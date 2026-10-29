#pragma once
#include <nt/iop_native.hpp>
#include <algorithm>

// Stable C ABI: 32 register words, PC, branch target, delay flag, pending load
// register/value, detail (fault address, BREAK code or native import ordinal), HI and LO. Counter is separate to avoid struct layout
// dependencies. All buffers belong to the caller; LLVM is used only at build time.
extern "C" std::uint32_t nt_iop_llvm_run_v2(std::uint32_t* state,std::uint8_t* ram,
    std::uint32_t ram_size,std::uint64_t* executed,std::uint64_t budget);

namespace nt::iop {
inline NativeResult run_llvm(Cpu& cpu,std::span<std::uint8_t> ram,std::uint64_t budget) {
    if (cpu.load_register>31) return {NativeStop::address_fault,cpu.pc};
    std::array<std::uint32_t,40> state{};
    std::copy(cpu.registers.begin(),cpu.registers.end(),state.begin());
    state[32]=cpu.pc; state[33]=cpu.branch_target; state[34]=cpu.delay_slot;
    state[35]=cpu.load_register; state[36]=cpu.load_value;
    state[38]=cpu.hi; state[39]=cpu.lo;
    const auto size=static_cast<std::uint32_t>(std::min<std::size_t>(ram.size(),0x200000));
    const auto stop=nt_iop_llvm_run_v2(state.data(),ram.data(),size,&cpu.executed,budget);
    std::copy_n(state.begin(),32,cpu.registers.begin());
    cpu.pc=state[32]; cpu.branch_target=state[33]; cpu.delay_slot=state[34]!=0;
    cpu.load_register=state[35]; cpu.load_value=state[36];
    cpu.hi=state[38]; cpu.lo=state[39];
    const auto reason=static_cast<NativeStop>(stop);
    const bool detail=reason==NativeStop::guest_break || reason==NativeStop::import_request;
    return {reason,cpu.pc,detail ? 0U : state[37],detail ? state[37] : 0U};
}
}
