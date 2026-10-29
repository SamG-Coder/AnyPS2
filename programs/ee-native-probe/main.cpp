#include <nt/ee_services.hpp>
#include <nt/ee_platform.hpp>
#include <nt/binary.hpp>
#include <charconv>
#include <iostream>
#include <stdexcept>
extern "C" nt::ee::Stop nt_ee_generated_run(nt::runtime::State*,nt::ee::Memory*,std::uint64_t);
extern "C" bool nt_ee_generated_code_overlap(std::uint32_t,std::uint32_t);
extern "C" bool nt_ee_generated_contains(std::uint32_t);
namespace {
bool trace_services=false;
nt::ee::Stop execute(nt::runtime::State* state,nt::ee::Memory* memory,std::uint64_t budget) {
    const auto stop=nt_ee_generated_run(state,memory,budget);
    if (trace_services && stop==nt::ee::Stop::syscall) {
        std::cerr<<"service pc="<<nt::hex32(state->pc)<<" number="<<state->gpr[3].lo;
        for (unsigned i=4;i<8;++i) std::cerr<<" a"<<(i-4)<<"="<<nt::hex32(static_cast<std::uint32_t>(state->gpr[i].lo));
        std::cerr<<'\n';
    }
    return stop;
}
}
int main(int argc,char** argv) {
    if (argc<2 || argc>4) {
        std::cerr<<"Usage: ps2-native <program.data> [operation-budget] [--trace-services]\n"; return 2;
    }
    try {
        std::uint64_t budget=1000000;
        if (argc>=3) {
            const std::string_view text=argv[2];
            const auto parsed=std::from_chars(text.data(),text.data()+text.size(),budget);
            if (parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size()) throw std::runtime_error("Invalid operation budget");
        }
        if (argc==4) {
            if (std::string_view(argv[3])!="--trace-services") throw std::runtime_error("Unknown probe option");
            trace_services=true;
        }
        auto ram=nt::read_file(argv[1],0x2000000);
        if (ram.size()!=ANYPS2_EE_DATA_SIZE || nt::fingerprint(ram)!=ANYPS2_EE_DATA_HASH)
            throw std::runtime_error("Converted program data mismatch");
        ram.resize(0x2000000,0);
        nt::ee::Memory memory{ram.data(),ram.size()};
        nt::ee::NativePlatform platform; platform.attach(memory);
        nt::runtime::State state; state.pc=ANYPS2_EE_ENTRY;
        nt::ee::StartupServices services(nt_ee_generated_code_overlap,nt_ee_generated_contains,ANYPS2_EE_SYSCALL_TABLE);
        const auto stop=nt::ee::run(execute,state,memory,budget,services);
        std::cout<<"stop="<<static_cast<unsigned>(stop)<<" pc="<<nt::hex32(state.pc)
                 <<" executed="<<state.executed<<" v0="<<state.gpr[2].lo<<" syscall="<<state.gpr[3].lo;
        if (stop==nt::ee::Stop::memory_fault || stop==nt::ee::Stop::code_write || stop==nt::ee::Stop::unpublished_code || (stop==nt::ee::Stop::unsupported_system && memory.fault_address))
            std::cout<<" address="<<nt::hex32(memory.fault_address);
        if (stop==nt::ee::Stop::unmapped_code && state.gpr[3].lo==0x74)
            std::cout<<" requested_syscall="<<state.gpr[4].lo
                     <<" uncompiled_handler="<<nt::hex32(static_cast<std::uint32_t>(state.gpr[5].lo));
        if (stop==nt::ee::Stop::unmapped_code && state.gpr[3].lo==0x20)
            std::cout<<" uncompiled_thread_entry="<<nt::hex32(memory.fault_address);
        if (trace_services) {
            std::cout<<" a0="<<nt::hex32(static_cast<std::uint32_t>(state.gpr[4].lo))
                     <<" a1="<<nt::hex32(static_cast<std::uint32_t>(state.gpr[5].lo));
        }
        std::cout<<'\n';
        return stop==nt::ee::Stop::budget ? 4 : 3;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
