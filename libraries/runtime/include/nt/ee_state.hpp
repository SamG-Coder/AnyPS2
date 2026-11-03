#pragma once
#include <array>
#include <cstdint>

namespace nt::runtime {
struct Register { std::uint64_t lo = 0, hi = 0; };
struct State {
    std::array<Register, 32> gpr{};
    std::array<std::uint32_t, 32> fpr{};
    std::uint32_t fcr31 = 0, accumulator = 0;
    std::uint32_t cop0_status = 0;
    std::uint32_t cop0_epc = 0, cop0_error_epc = 0;
    std::uint64_t hi = 0, lo = 0, executed = 0;
    std::uint64_t hi1 = 0, lo1 = 0, sa = 0;
    std::uint32_t pc = 0, pending_target = 0, branch_pc = 0;
    bool delay_slot = false;
    bool accumulator_overflow = false;
    void set(unsigned reg, std::uint64_t value) { if (reg) gpr[reg].lo = value; }
    void set128(unsigned reg, Register value) { if (reg) gpr[reg] = value; }
};
}
