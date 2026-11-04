#include <nt/ee_platform.hpp>
#include <memory>
#include <source_location>
extern "C" nt::ee::Stop nt_ee_generated_run(nt::runtime::State*,nt::ee::Memory*,std::uint64_t);
extern "C" bool nt_ee_generated_code_overlap(std::uint32_t,std::uint32_t);
namespace {
void check(bool value,std::source_location location=std::source_location::current()) {
    if (!value) throw std::runtime_error("Graphics readback regression at line "+std::to_string(location.line()));
}
}
void run_graphics_readback_tests() {
    std::vector<std::uint8_t> ram(0x400000,0xcc);
    nt::ee::Memory memory{ram.data(),ram.size()};
    auto owner=std::make_unique<nt::ee::NativePlatform>(); auto& platform=*owner; platform.attach(memory);
    platform.packet_submission.protection=nt_ee_generated_code_overlap;
    struct Source { unsigned remaining=32,calls=0; bool fail=false; } source;
    platform.graphics.context=&source;
    platform.graphics.synchronize=[](void*) { return nt::ee::Stop::budget; };
    platform.graphics.readback_status=[](void* raw) -> nt::ee::IoRead {
        return {nt::ee::Stop::budget,static_cast<Source*>(raw)->remaining};
    };
    platform.graphics.readback=[](void* raw,std::span<std::uint8_t> output) {
        auto& s=*static_cast<Source*>(raw);
        if (s.fail || output.size()>s.remaining) return nt::ee::Stop::unsupported_system;
        for (unsigned i=0;i<output.size();++i) output[i]=std::uint8_t(32-s.remaining+i);
        s.remaining-=static_cast<unsigned>(output.size()); ++s.calls; return nt::ee::Stop::budget;
    };
    const auto write=[&](unsigned address,unsigned value) {
        nt::runtime::State state; state.pc=0x360000; state.set(4,address); state.set(8,value);
        return nt_ee_generated_run(&state,&memory,1);
    };
    check(write(0x10009010,0x80001000)==nt::ee::Stop::budget); // scratchpad destination
    check(write(0x10009020,1)==nt::ee::Stop::budget);
    check(write(0x10009000,0x100)==nt::ee::Stop::unsupported_system && !source.calls);
    check(write(0x10003c00,0x800000)==nt::ee::Stop::budget);
    check(write(0x12001040,1)==nt::ee::Stop::budget);
    const auto status=memory.io.read32(memory.io.context,0x10003c00);
    check(status.status==nt::ee::Stop::budget && (status.value>>24)==2 && (status.value&0x800000));
    source.fail=true;
    check(write(0x10009000,0x100)==nt::ee::Stop::unsupported_system && !platform.completions.status());
    source.fail=false;
    check(write(0x10009010,0x360000)==nt::ee::Stop::budget);
    check(write(0x10009000,0x100)==nt::ee::Stop::code_write && !source.calls);
    check(write(0x10009010,0x80001000)==nt::ee::Stop::budget);
    check(write(0x10009000,0x100)==nt::ee::Stop::budget && source.calls==1);
    for (unsigned i=0;i<16;++i) check(memory.scratchpad[0x1000+i]==i);
    check((platform.completions.status()&2) && platform.packet_submission.read(0x10009020).value==0);
    platform.completions.acknowledge(2);
    check(write(0x10009010,0x201fe000)==nt::ee::Stop::budget); // cached/uncached alias normalization
    check(write(0x10009020,1)==nt::ee::Stop::budget);
    check(write(0x10009000,0xbeef01c0)==nt::ee::Stop::budget && source.remaining==0);
    check(platform.packet_submission.read(0x10009000).value==0xbeef00c0);
    for (unsigned i=0;i<16;++i) check(ram[0x1fe000+i]==16+i);
    check((memory.io.read32(memory.io.context,0x10003c00).value>>24)==0);
    check(write(0x12001040,0)==nt::ee::Stop::budget && write(0x10003c00,0)==nt::ee::Stop::budget);
}
