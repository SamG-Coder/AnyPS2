#pragma once
#include <nt/iop_module.hpp>
#include <map>
#include <set>
#include <string>
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
    add_checked, subtract_checked, add_immediate_checked, memory_fill, memory_copy, memory_move, native_import,
    load_word_left, load_word_right, store_word_left, store_word_right
};
enum class Flow { sequential, direct, conditional, call, return_register, trap, service, indirect, indirect_call, native_return };
enum class MemoryConvention { bytes, source_first, words };
struct Instruction {
    Operation operation{};
    Flow flow=Flow::sequential;
    std::uint32_t address=0, immediate=0, target=0;
    std::vector<std::uint32_t> targets; // AOT destinations, checked against the actual indirect target.
    unsigned source=0, operand=0, destination=0, shift=0, width=0;
    MemoryConvention memory_convention=MemoryConvention::bytes;
};
struct Program {
    std::uint32_t entry=0;
    std::map<std::uint32_t,Instruction> instructions;
    std::set<std::uint32_t> protected_words;
    std::vector<elf::IopImport> library_imports; // Absolute stub/table addresses after analysis.
    std::set<std::uint32_t> guarded_callback_sites; // Bounded address-taken candidates, not exhaustive proofs.
    std::set<std::uint32_t> open_indirect_sites; // One fixed slot with an unbounded store, not a proved target list. A callee queued only by that proof is removed.
};
Program discover(const elf::IopModule& module,std::span<const std::uint32_t> entries={});
Program discover_bundle(std::span<const elf::IopModule> modules,std::span<const std::uint32_t> entries={});
// Collected conversion gaps. Kind distinguishes a missing service from an
// untranslated instruction so later batches can be chosen by subsystem.
struct Gap {
    enum class Kind { import_missing, instruction, indirect, unresolved_target, limit, control };
    Kind kind=Kind::control;
    std::string library;
    std::uint16_t version=0;
    std::uint16_t ordinal=0;
    std::uint32_t address=0;
    std::uint32_t word=0;
    std::string detail;
};
struct ModuleDiagnosis {
    Program program;
    std::vector<Gap> gaps;
    std::string unexpected;
    bool convertible() const { return gaps.empty() && unexpected.empty() && !program.instructions.empty(); }
};
// Analyze every module. A failure is stored on that module and does not cancel
// the others. Sibling export tables still bind imports of modules that convert.
std::vector<ModuleDiagnosis> diagnose_bundle(std::span<const elf::IopModule> modules,std::span<const std::uint32_t> entries={});
}
