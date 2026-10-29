#include <nt/ee_services.hpp>
#include <nt/binary.hpp>
#include <charconv>
#include <iostream>
#include <stdexcept>
extern "C" nt::ee::Stop nt_ee_generated_run(nt::runtime::State*,nt::ee::Memory*,std::uint64_t);
extern "C" bool nt_ee_generated_code_overlap(std::uint32_t,std::uint32_t);
int main(int argc,char** argv) {
    if (argc<2 || argc>3) {
        std::cerr<<"Usage: ps2-native <program.data> [operation-budget]\n"; return 2;
    }
    try {
        std::uint64_t budget=1000000;
        if (argc==3) {
            const std::string_view text=argv[2];
            const auto parsed=std::from_chars(text.data(),text.data()+text.size(),budget);
            if (parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size()) throw std::runtime_error("Invalid operation budget");
        }
        auto ram=nt::read_file(argv[1],0x2000000);
        if (ram.size()!=ANYPS2_EE_DATA_SIZE || nt::fingerprint(ram)!=ANYPS2_EE_DATA_HASH)
            throw std::runtime_error("Converted program data mismatch");
        ram.resize(0x2000000,0);
        nt::ee::Memory memory{ram.data(),ram.size()};
        nt::runtime::State state; state.pc=ANYPS2_EE_ENTRY;
        nt::ee::StartupServices services(nt_ee_generated_code_overlap);
        const auto stop=nt::ee::run(nt_ee_generated_run,state,memory,budget,services);
        std::cout<<"stop="<<static_cast<unsigned>(stop)<<" pc="<<nt::hex32(state.pc)
                 <<" executed="<<state.executed<<" v0="<<state.gpr[2].lo<<" syscall="<<state.gpr[3].lo;
        if (stop==nt::ee::Stop::memory_fault || stop==nt::ee::Stop::code_write)
            std::cout<<" address="<<nt::hex32(memory.fault_address);
        std::cout<<'\n';
        return stop==nt::ee::Stop::budget ? 4 : 3;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
