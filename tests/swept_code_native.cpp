#include <nt/ee_native.hpp>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
extern "C" nt::ee::Stop nt_ee_generated_run(nt::runtime::State*,nt::ee::Memory*,std::uint64_t);
extern "C" bool nt_ee_generated_code_overlap(std::uint32_t,std::uint32_t);
namespace {
void check(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
// Matches emit_swept_code_fixture.cpp: jr a0 at 0x1000 is proven and the
// routine at 0x1010 is swept. Words are host-endian on the supported target.
std::vector<std::uint8_t> image() {
    std::vector<std::uint8_t> bytes(0x2000,0);
    const auto put=[&](std::uint32_t address,std::uint32_t word) { std::memcpy(bytes.data()+address,&word,4); };
    put(0x1000,0x00800008); put(0x1010,0x2402002a); put(0x1014,0x03e00008); put(0x1030,12);
    return bytes;
}
nt::ee::Stop run(std::vector<std::uint8_t>* bytes,nt::runtime::State& state,nt::ee::Memory& memory) {
    if (bytes) { memory.data=bytes->data(); memory.size=bytes->size(); }
    state.pc=0x1000; state.set(4,0x1010); state.set(31,0x1030);
    return nt_ee_generated_run(&state,&memory,20);
}
}
int main() {
    try {
        {
            auto bytes=image(); nt::runtime::State state; nt::ee::Memory memory;
            check(run(&bytes,state,memory)==nt::ee::Stop::syscall && state.pc==0x1030 && state.gpr[2].lo==42,
                  "Intact swept routine did not run natively");
        }
        // Each changed word stops before it can run, including a delay slot.
        for (const auto address:std::array<std::uint32_t,3>{0x1010,0x1014,0x1018}) {
            auto bytes=image(); bytes[address]^=1;
            nt::runtime::State state; nt::ee::Memory memory;
            check(run(&bytes,state,memory)==nt::ee::Stop::unpublished_code && memory.fault_address==address,
                  "Changed swept word ran stale native code");
        }
        {
            nt::runtime::State state; nt::ee::Memory memory;
            check(run(nullptr,state,memory)==nt::ee::Stop::unpublished_code && memory.fault_address==0x1010,
                  "Swept routine ran without guest memory");
        }
        check(nt_ee_generated_code_overlap(0x1000,4),"Proven code lost write protection");
        check(!nt_ee_generated_code_overlap(0x1010,12),"Swept words were write-protected");
        std::cout<<"Swept code guards passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
