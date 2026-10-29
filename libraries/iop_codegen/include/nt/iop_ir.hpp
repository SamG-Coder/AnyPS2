#pragma once
#include <nt/iop_module.hpp>
#include <map>
#include <set>
#include <vector>

namespace nt::iop_codegen::ir {
// Initial semantic IR for IOP conversion. It contains no emitted source strings
// or raw instruction words. Delay/load ordering remains explicit for backends.
enum class Operation {
    shift_left, shift_right, shift_arithmetic, shift_left_variable, shift_right_variable,
    shift_arithmetic_variable, return_register, read_hi, write_hi, read_lo, write_lo,
    multiply_signed, multiply_unsigned, divide_signed, divide_unsigned, add, subtract,
    bit_and, bit_or, bit_xor, bit_nor, less_signed, less_unsigned,
    jump, call, equal, not_equal, negative, nonnegative, nonpositive, positive,
    add_immediate, less_immediate_signed, less_immediate_unsigned,
    and_immediate, or_immediate, xor_immediate, upper_immediate,
    load_byte, load_half, load_word, load_byte_unsigned, load_half_unsigned,
    store_byte, store_half, store_word, breakpoint, syscall, jump_register, call_register,
    add_checked, subtract_checked, add_immediate_checked, memory_fill, memory_copy, memory_move, native_import
};
enum class Flow { sequential, direct, conditional, call, return_register, trap, service, indirect, indirect_call, native_return };
struct Instruction {
    Operation operation{};
    Flow flow=Flow::sequential;
    std::uint32_t address=0, immediate=0, target=0;
    std::vector<std::uint32_t> targets; // Proven destinations for an indirect transfer.
    unsigned source=0, operand=0, destination=0, shift=0, width=0;
};
struct Program {
    std::uint32_t entry=0;
    std::map<std::uint32_t,Instruction> instructions;
    std::set<std::uint32_t> protected_words;
    std::vector<elf::IopImport> library_imports; // Absolute stub/table addresses after analysis.
};
Program discover(const elf::IopModule& module,std::span<const std::uint32_t> entries={});
Program discover_bundle(std::span<const elf::IopModule> modules,std::span<const std::uint32_t> entries={});
}
