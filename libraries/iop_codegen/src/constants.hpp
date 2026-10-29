#pragma once
#include <nt/iop_ir.hpp>
#include <array>
#include <optional>
#include <bit>
#include <limits>

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
    std::array<std::optional<std::int32_t>,32> stack_offsets{};
    std::map<std::int32_t,Constant> stack_words;
    Constants() { registers[0]=constant(0); stack_offsets[29]=0; }
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
        for (unsigned r=1;r<32;++r) if (stack_offsets[r] && stack_offsets[r]!=incoming.stack_offsets[r]) {
            stack_offsets[r].reset(); changed=true;
        }
        for (auto word=stack_words.begin();word!=stack_words.end();) {
            const auto other=incoming.stack_words.find(word->first);
            if (other==incoming.stack_words.end() || !other->second) { word=stack_words.erase(word); changed=true; continue; }
            auto values=*word->second;
            values.insert(other->second->begin(),other->second->end());
            if (values.size()>value_limit) { word=stack_words.erase(word); changed=true; continue; }
            if (values!=*word->second) { word->second=std::move(values); changed=true; }
            ++word;
        }
        return changed;
    }
};
inline Constants transfer(const Instruction& i,Constants state) {
    const auto a=state.registers[i.source], b=state.registers[i.operand];
    const auto source_stack=state.stack_offsets[i.source],operand_stack=state.stack_offsets[i.operand];
    const auto write=[&](unsigned reg,Constant value={}) {
        if (reg) { state.registers[reg]=value; state.stack_offsets[reg].reset(); }
    };
    const auto offset=[&](std::optional<std::int32_t> base,std::uint32_t displacement)->std::optional<std::int32_t> {
        if (!base) return {};
        const auto value=std::int64_t(*base)+std::bit_cast<std::int32_t>(displacement);
        if (value<std::numeric_limits<std::int32_t>::min() || value>std::numeric_limits<std::int32_t>::max()) return {};
        return static_cast<std::int32_t>(value);
    };
    switch (i.operation) {
    case Operation::upper_immediate: write(i.operand,constant(i.immediate<<16)); break;
    case Operation::add_immediate:
        write(i.operand,combine(a,constant(i.immediate),[](auto x,auto y) { return x+y; }));
        if (i.operand) state.stack_offsets[i.operand]=offset(source_stack,i.immediate);
        break;
    case Operation::add_immediate_checked: write(i.operand); break;
    case Operation::add_checked: case Operation::subtract_checked: write(i.destination); break;
    case Operation::and_immediate: write(i.operand,combine(a,constant(i.immediate&65535),[](auto x,auto y) { return x&y; })); break;
    case Operation::or_immediate: write(i.operand,combine(a,constant(i.immediate&65535),[](auto x,auto y) { return x|y; })); break;
    case Operation::xor_immediate: write(i.operand,combine(a,constant(i.immediate&65535),[](auto x,auto y) { return x^y; })); break;
    case Operation::add:
        write(i.destination,combine(a,b,[](auto x,auto y) { return x+y; }));
        if (i.destination && b && b->size()==1) state.stack_offsets[i.destination]=offset(source_stack,*b->begin());
        if (i.destination && a && a->size()==1 && !state.stack_offsets[i.destination])
            state.stack_offsets[i.destination]=offset(operand_stack,*a->begin());
        break;
    case Operation::subtract: write(i.destination,combine(a,b,[](auto x,auto y) { return x-y; })); break;
    case Operation::bit_or: write(i.destination,combine(a,b,[](auto x,auto y) { return x|y; })); break;
    case Operation::bit_and: write(i.destination,combine(a,b,[](auto x,auto y) { return x&y; })); break;
    case Operation::bit_xor: write(i.destination,combine(a,b,[](auto x,auto y) { return x^y; })); break;
    case Operation::store_byte: case Operation::store_half: case Operation::store_word: {
        const auto address=offset(source_stack,i.immediate);
        if (!address) { state.stack_words.clear(); break; }
        const auto bytes=i.operation==Operation::store_word ? 4 : i.operation==Operation::store_half ? 2 : 1;
        std::erase_if(state.stack_words,[&](const auto& word) {
            return std::int64_t(word.first)<std::int64_t(*address)+bytes && std::int64_t(*address)<std::int64_t(word.first)+4;
        });
        if (i.operation==Operation::store_word && *address%4==0 && b) {
            if (state.stack_words.size()==64) state.stack_words.clear();
            state.stack_words.emplace(*address,b);
        }
        break;
    }
    case Operation::call: write(31); break;
    case Operation::call_register: write(i.destination); break;
    case Operation::native_import: case Operation::syscall: case Operation::memory_fill: case Operation::memory_copy: case Operation::memory_move: return Constants{};
    case Operation::shift_left: case Operation::shift_right: case Operation::shift_arithmetic:
    case Operation::shift_left_variable: case Operation::shift_right_variable: case Operation::shift_arithmetic_variable: {
        const bool variable=i.operation==Operation::shift_left_variable || i.operation==Operation::shift_right_variable ||
                            i.operation==Operation::shift_arithmetic_variable;
        write(i.destination,combine(b,variable ? a : constant(i.shift),[&](std::uint32_t value,std::uint32_t amount) {
            const auto shift=amount&31U;
            if (i.operation==Operation::shift_left || i.operation==Operation::shift_left_variable) return value<<shift;
            const auto logical=value>>shift;
            if (shift && (value&0x80000000U) &&
                (i.operation==Operation::shift_arithmetic || i.operation==Operation::shift_arithmetic_variable))
                return logical | (~0U<<(32-shift));
            return logical;
        }));
        break;
    }
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
