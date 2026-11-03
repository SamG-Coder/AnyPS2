#include <nt/ee_platform.hpp>
#include <array>
#include <cstring>
#include <source_location>
#include <stdexcept>
#include <string>

extern "C" nt::ee::Stop nt_ee_generated_run(nt::runtime::State*,nt::ee::Memory*,std::uint64_t);
namespace {
void check(bool ok,std::source_location location=std::source_location::current()) {
    if (!ok) throw std::runtime_error("Scratchpad copy mismatch at line "+std::to_string(location.line()));
}
void word(std::uint8_t* bytes,unsigned offset,std::uint32_t value) { std::memcpy(bytes+offset,&value,4); }
}
void run_scratchpad_tests() {
    using nt::ee::Stop;
    std::array<std::uint8_t,1024> ram{};
    for (unsigned n=0;n<128;++n) ram[n]=static_cast<std::uint8_t>(n);
    nt::ee::Memory memory{ram.data(),ram.size()}; nt::ee::NativePlatform platform; platform.attach(memory);
    platform.scratchpad_copy.protection=[](std::uint32_t,std::uint32_t) { return false; };
    nt::runtime::State state;
    const auto write=[&](unsigned address,unsigned value) {
        state.pc=0x360000; state.set(4,address); state.set(8,value);
        return nt_ee_generated_run(&state,&memory,1);
    };
    const auto read=[&](unsigned address) {
        state.pc=0x360008; state.set(4,address);
        check(nt_ee_generated_run(&state,&memory,1)==Stop::budget);
        return static_cast<unsigned>(state.gpr[8].lo);
    };
    const auto set=[&](unsigned base,unsigned source,unsigned count,unsigned scratch) {
        check(write(base+0x10,source)==Stop::budget);
        check(write(base+0x20,count)==Stop::budget);
        check(write(base+0x80,scratch)==Stop::budget);
    };
    const auto clear=[&] { check(write(0x1000e010,0x300)==Stop::budget); };
    constexpr unsigned from=0x1000d000,to=0x1000d400;
    // Normal copies use the same backing scratchpad as compiled loads/stores.
    check(write(0x1000e010,1U<<25)==Stop::budget);
    set(to,0x80000040,2,0x80003ff0);
    check(write(to,0x101)==Stop::budget);
    check(std::memcmp(memory.scratchpad+0x3ff0,ram.data()+64,16)==0);
    check(std::memcmp(memory.scratchpad,ram.data()+80,16)==0);
    check(read(to+0x80)==16 && read(to+0x10)==0x80000060 && !read(to+0x20));
    check(!(read(to)&0x100) && (platform.completions.status()&0x200) && (memory.pending_interrupts&0x800));
    clear(); set(from,0xa0000080,2,0x3ff0);
    check(write(from,0x100)==Stop::budget && std::memcmp(ram.data()+128,ram.data()+64,32)==0);
    check(read(from+0x80)==16 && (platform.completions.status()&0x100));
    clear();
    // Source REF + END includes complete 16-byte descriptors when TTE is set.
    word(ram.data(),0x100,0x30000001); word(ram.data(),0x104,32);
    word(ram.data(),0x108,0x11223344); word(ram.data(),0x10c,0x55667788);
    word(ram.data(),0x110,0x70000001); word(ram.data(),0x114,0);
    std::fill(ram.begin()+0x120,ram.begin()+0x130,0x91);
    set(to,0,0,0x3ff0); check(write(to+0x30,0x100)==Stop::budget);
    check(write(to,0x145)==Stop::budget);
    check(std::memcmp(memory.scratchpad+0x3ff0,ram.data()+0x100,16)==0);
    check(std::memcmp(memory.scratchpad,ram.data()+32,16)==0);
    check(std::memcmp(memory.scratchpad+16,ram.data()+0x110,32)==0);
    check(read(to+0x80)==48 && read(to+0x30)==0x110 && read(to+0x10)==0x130);
    clear();
    // Preflight rejection after a valid descriptor must not publish partial data.
    std::array<std::uint8_t,nt::ee::scratchpad_size> snapshot;
    std::copy_n(memory.scratchpad,snapshot.size(),snapshot.begin());
    word(ram.data(),0x110,0x00000001); word(ram.data(),0x114,1024);
    set(to,0,0,0); check(write(to+0x30,0x100)==Stop::budget);
    const auto control=read(to);
    check(write(to,0x145)==Stop::memory_fault && read(to)==control && read(to+0x80)==0);
    check(std::equal(snapshot.begin(),snapshot.end(),memory.scratchpad) && !(platform.completions.status()&0x300));
    check(write(to,0x109)==Stop::unsupported_system);
    check(write(0x1000e000,0)==Stop::budget && write(to,0x101)==Stop::unsupported_system);
    check(write(0x1000e000,1)==Stop::budget);
    // TIE ends a valid descriptor batch without requiring a subsequent tag.
    word(ram.data(),0x100,0xb0000001); set(to,0,0,0);
    check(write(to,0x185)==Stop::budget && read(to+0x30)==0x110 && read(to+0x80)==16);
    clear();
    // Destination chains consume scratchpad descriptors and copy in order.
    word(memory.scratchpad,0x3ff0,0x10000001); word(memory.scratchpad,0x3ff4,0x180);
    std::fill_n(memory.scratchpad,16,0x51);
    word(memory.scratchpad,16,0x70000001); word(memory.scratchpad,20,0x1a0);
    std::fill_n(memory.scratchpad+32,16,0xa7);
    set(from,0,0,0x3ff0);
    check(write(from,0x104)==Stop::budget && ram[0x180]==0x51 && ram[0x1af]==0xa7);
    check(read(from+0x80)==48 && read(from+0x10)==0x1b0);
    clear();
    std::fill(ram.begin()+0x180,ram.begin()+0x1b0,0x33);
    word(memory.scratchpad,20,1024); set(from,0,0,0x3ff0);
    check(write(from,0x104)==Stop::memory_fault && ram[0x180]==0x33 && read(from+0x80)==0x3ff0);
    check(!(platform.completions.status()&0x300));
    // All native writes require the generated code-protection predicate.
    platform.scratchpad_copy.protection=[](std::uint32_t address,std::uint32_t bytes) { return address<0x1a0 && address+bytes>0x180; };
    set(from,0x180,1,0);
    check(write(from,0x100)==Stop::code_write && ram[0x180]==0x33 && read(from+0x20)==1);
    check(memory.fault_address==0x180);
    platform.scratchpad_copy.protection=nullptr;
    check(write(from,0x100)==Stop::invalid_state);
    // A cyclic descriptor list is bounded; no bytes or completion escape.
    word(ram.data(),0x100,0x20000000); word(ram.data(),0x104,0x100);
    set(to,0,0,0); check(write(to+0x30,0x100)==Stop::budget);
    check(write(to,0x105)==Stop::invalid_state && !(platform.completions.status()&0x300));
}
