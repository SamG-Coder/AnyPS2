#include <nt/ee_native.hpp>
#include <iostream>
#include <stdexcept>
extern "C" nt::ee::Stop nt_ee_generated_run(nt::runtime::State*,nt::ee::Memory*,std::uint64_t);
extern "C" bool nt_ee_generated_contains(std::uint32_t);
int main() {
    try {
        for (const auto target:{0x1010U,0x1020U,0x1010U}) {
            nt::runtime::State state; nt::ee::Memory memory;
            state.pc=0x1000; state.set(4,target); state.set(31,0x1030);
            if (nt_ee_generated_run(&state,&memory,20)!=nt::ee::Stop::syscall ||
                state.pc!=0x1030 || state.gpr[2].lo!=(target==0x1010 ? 42U : 78U))
                throw std::runtime_error("Native indirect call did not use the supplied runtime destination and delay slot");
        }
        for (const auto target:{0x1040U,0x1050U,0x1060U,0x1070U,0x1011U}) {
            nt::runtime::State state; nt::ee::Memory memory;
            state.pc=0x1000; state.set(4,target);
            if (nt_ee_generated_contains(target) ||
                nt_ee_generated_run(&state,&memory,20)!=nt::ee::Stop::unmapped_code || state.pc!=target)
                throw std::runtime_error("Data, VU, padding, BSS or unaligned address accepted as native EE code");
        }
        std::cout<<"Whole-code native runtime-selected callbacks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
