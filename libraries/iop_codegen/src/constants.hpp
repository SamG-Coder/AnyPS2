#pragma once
#include <nt/iop_ir.hpp>
#include <array>
#include <optional>

namespace nt::iop_codegen::ir {
// A bounded value set is an over-approximation, never a guess at a pointer.
// Unknown inputs or excessive growth lose the proof instead of dropping targets.
inline constexpr std::size_t value_limit=16;
using Constant=std::optional<std::set<std::uint32_t>>;
inline Constant constant(std::uint32_t value) { return std::set<std::uint32_t>{value}; }
template<class Operation>
Constant combine(const Constant& a,const Constant& b,Operation operation) {
    if (!a || !b) return {};
    std::set<std::uint32_t> result;
    for (const auto left : *a) for (const auto right : *b) {
        result.insert(operation(left,right));
        if (result.size()>value_limit) return {};
    }
    return result;
}
struct Constants {
    std::array<Constant,32> registers{};
    Constants() { registers[0]=constant(0); }
    bool merge(const Constants& incoming) {
        bool changed=false;
        for (unsigned r=1;r<32;++r) {
            auto& current=registers[r];
            if (!current) continue;
            if (!incoming.registers[r]) { current.reset(); changed=true; continue; }
            const auto before=current->size();
            current->insert(incoming.registers[r]->begin(),incoming.registers[r]->end());
            if (current->size()!=before) changed=true;
            if (current->size()>value_limit) current.reset();
        }
        return changed;
    }
};
inline Constants transfer(const Instruction& i,Constants state) {
    const auto a=state.registers[i.source], b=state.registers[i.operand];
    const auto write=[&](unsigned reg,Constant value={}) { if (reg) state.registers[reg]=value; };
    switch (i.operation) {
    case Operation::upper_immediate: write(i.operand,constant(i.immediate<<16)); break;
    case Operation::add_immediate: write(i.operand,combine(a,constant(i.immediate),[](auto x,auto y) { return x+y; })); break;
    case Operation::add_immediate_checked: write(i.operand); break;
    case Operation::add_checked: case Operation::subtract_checked: write(i.destination); break;
    case Operation::and_immediate: write(i.operand,combine(a,constant(i.immediate&65535),[](auto x,auto y) { return x&y; })); break;
    case Operation::or_immediate: write(i.operand,combine(a,constant(i.immediate&65535),[](auto x,auto y) { return x|y; })); break;
    case Operation::xor_immediate: write(i.operand,combine(a,constant(i.immediate&65535),[](auto x,auto y) { return x^y; })); break;
    case Operation::add: write(i.destination,combine(a,b,[](auto x,auto y) { return x+y; })); break;
    case Operation::subtract: write(i.destination,combine(a,b,[](auto x,auto y) { return x-y; })); break;
    case Operation::bit_or: write(i.destination,combine(a,b,[](auto x,auto y) { return x|y; })); break;
    case Operation::bit_and: write(i.destination,combine(a,b,[](auto x,auto y) { return x&y; })); break;
    case Operation::bit_xor: write(i.destination,combine(a,b,[](auto x,auto y) { return x^y; })); break;
    case Operation::call: write(31); break;
    case Operation::call_register: write(i.destination); break;
    case Operation::native_import: case Operation::syscall: case Operation::memory_fill: case Operation::memory_copy: case Operation::memory_move: return Constants{};
    case Operation::shift_left: case Operation::shift_right: case Operation::shift_arithmetic:
    case Operation::shift_left_variable: case Operation::shift_right_variable: case Operation::shift_arithmetic_variable:
    case Operation::less_signed: case Operation::less_unsigned: case Operation::bit_nor:
    case Operation::read_hi: case Operation::read_lo: write(i.destination); break;
    case Operation::less_immediate_signed: case Operation::less_immediate_unsigned: write(i.operand); break;
    case Operation::load_byte: case Operation::load_half: case Operation::load_word:
    case Operation::load_byte_unsigned: case Operation::load_half_unsigned:
        // Forget immediately rather than assume a memory value or load timing.
        // This sacrifices some proofs but never turns mutable data into code.
        write(i.operand); break;
    default: break;
    }
    return state;
}
}
