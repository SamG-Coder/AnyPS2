#pragma once
#include "constants.hpp"
#include <map>
#include <set>
#include <span>
#include <vector>

namespace nt::iop_codegen::ir {
// A word loaded from one constant address is an indirect target only when every
// store that can write that address puts a bounded executable pointer there and
// the initial word is zero or one of those pointers. Caller-saved and
// callee-saved registers are both discarded across a call. The delay slot of a
// jump is not a fallthrough into the next instruction. An unknown absolute
// address or a memory fill/copy/move drops every proof in the module that
// contains that instruction. A stack-relative store is not an absolute slot.
// Partial overlaps and non-executable stored values leave the site unresolved.
// Runtime transfer still rejects a pointer outside this set.
inline std::map<std::uint32_t,std::set<std::uint32_t>> recoverable_loaded_pointers(
    const Program& program,const std::map<std::uint32_t,Constants>& states,std::span<const elf::IopModule> modules) {
    const auto physical=[](std::uint32_t address) {
        const auto segment=address&0xe0000000U;
        return (segment==0x80000000U || segment==0xa0000000U) ? address&0x1fffffffU : address;
    };
    const auto executable=[&](std::uint32_t address) {
        address=physical(address);
        if (!address || address%4) return false;
        for (const auto& owner : modules) {
            const auto text=owner.info ? owner.info->text_size : owner.initialized_size;
            if (address>=owner.base && address-owner.base<text) return true;
        }
        return false;
    };
    const auto image_word=[&](std::uint32_t address)->std::optional<std::uint32_t> {
        if (address%4) return {};
        for (const auto& owner : modules) {
            if (address<owner.base) continue;
            const auto offset=address-owner.base;
            if (offset>owner.memory.size() || owner.memory.size()-offset<4) continue;
            if (offset>=owner.initialized_size || owner.initialized_size-offset<4) return 0U;
            return le32(owner.memory,offset);
        }
        return {};
    };
    const auto one_address=[&](const Constants& state,unsigned base,std::uint32_t displacement)->std::optional<std::uint32_t> {
        const auto& values=state.registers[base];
        if (!values || values->size()!=1) return {};
        return physical(*values->begin()+displacement);
    };
    bool wild=false;
    std::vector<char> module_wild(modules.size());
    const auto module_of=[&](std::uint32_t address)->std::optional<std::size_t> {
        for (std::size_t index=0;index<modules.size();++index) {
            const auto& owner=modules[index];
            if (address>=owner.base && address-owner.base<owner.memory.size()) return index;
        }
        return {};
    };
    const auto mark_wild=[&](std::uint32_t pc) {
        const auto owner=module_of(pc);
        if (!owner) { wild=true; return; }
        module_wild[*owner]=1;
    };
    std::set<std::uint32_t> poisoned;
    std::map<std::uint32_t,std::set<std::uint32_t>> stored;
    const auto poison_word=[&](std::uint32_t address) {
        const auto word=address&~3U;
        poisoned.insert(word); stored.erase(word);
    };
    const auto poison_span=[&](std::uint32_t address,unsigned width) {
        if (!width || address>0xffffffffU-(width-1)) { wild=true; return; }
        const auto last=(address+width-1)&~3U;
        for (auto word=address&~3U;;word+=4) {
            poison_word(word);
            if (word==last) break;
        }
    };
    const auto add_code=[&](std::uint32_t value,std::set<std::uint32_t>& targets) {
        value=physical(value);
        if (!value) return true;
        if (!executable(value) || targets.size()==value_limit) return false;
        targets.insert(value); return true;
    };
    std::map<std::uint32_t,Constants> delay_states;
    const auto state_at=[&](std::uint32_t pc)->const Constants* {
        if (const auto found=states.find(pc); found!=states.end()) return &found->second;
        if (pc<4) return nullptr;
        const auto branch=program.instructions.find(pc-4);
        if (branch==program.instructions.end() || !states.contains(pc-4)) return nullptr;
        const auto flow=branch->second.flow;
        if (flow!=Flow::direct && flow!=Flow::conditional && flow!=Flow::call && flow!=Flow::indirect &&
            flow!=Flow::indirect_call && flow!=Flow::return_register) return nullptr;
        return &delay_states.emplace(pc,transfer(branch->second,states.at(pc-4))).first->second;
    };
    for (const auto& [pc,instruction] : program.instructions) {
        if (instruction.operation==Operation::memory_fill || instruction.operation==Operation::memory_copy ||
            instruction.operation==Operation::memory_move) { mark_wild(pc); if (wild) break; continue; }
        const bool store=instruction.operation==Operation::store_byte || instruction.operation==Operation::store_half ||
            instruction.operation==Operation::store_word || instruction.operation==Operation::store_word_left ||
            instruction.operation==Operation::store_word_right;
        if (!store) continue;
        const auto state=state_at(pc);
        const auto addresses=state ? state->registers[instruction.source] : Constant{};
        if (!state || !addresses || addresses->empty() || addresses->size()>value_limit) {
            // sp-relative spills stay on the stack lattice and do not alias a fixed slot.
            if (!(state && state->stack_offsets[instruction.source])) mark_wild(pc);
            if (wild) break;
            continue;
        }
        const unsigned width=instruction.operation==Operation::store_byte ? 1U : instruction.operation==Operation::store_half ? 2U : 4U;
        for (const auto base : *addresses) {
            const auto address=physical(base+instruction.immediate);
            if (instruction.operation!=Operation::store_word || address%4) { poison_span(address,width); continue; }
            if (poisoned.contains(address)) continue;
            const auto& values=state->registers[instruction.operand];
            auto& targets=stored[address];
            if (!values) { poison_word(address); continue; }
            for (const auto value : *values) if (!add_code(value,targets)) { poison_word(address); break; }
            if (stored.contains(address) && stored[address].size()>value_limit) poison_word(address);
        }
    }
    if (wild) return {};
    struct Cell {
        enum class Kind : std::uint8_t { none, slot, unknown };
        Kind kind=Kind::none; std::uint32_t slot=0;
        bool operator==(const Cell&) const=default;
        static Cell slot_of(std::uint32_t address) { return {Kind::slot,address}; }
        static Cell unknown_cell() { return {Kind::unknown,0}; }
    };
    struct Id {
        std::array<Cell,32> regs{};
        std::array<Cell,32> delayed{};
        Id() { for (auto& reg : regs) reg=Cell::unknown_cell(); }
        bool operator==(const Id&) const=default;
    };
    const auto merge_cell=[](Cell left,Cell right,bool delay) {
        if (delay && left.kind==Cell::Kind::none) return right;
        if (delay && right.kind==Cell::Kind::none) return left;
        if (left.kind==Cell::Kind::slot && right.kind==Cell::Kind::slot && left.slot==right.slot) return left;
        return Cell::unknown_cell();
    };
    const auto merge_id=[&](const Id& left,const Id& right) {
        Id merged;
        for (unsigned reg=0;reg<32;++reg) {
            merged.regs[reg]=merge_cell(left.regs[reg],right.regs[reg],false);
            merged.delayed[reg]=merge_cell(left.delayed[reg],right.delayed[reg],true);
        }
        return merged;
    };
    const auto written_register=[](const Instruction& instruction)->unsigned {
        switch (instruction.operation) {
        case Operation::upper_immediate: case Operation::add_immediate: case Operation::add_immediate_checked:
        case Operation::less_immediate_signed: case Operation::less_immediate_unsigned:
        case Operation::and_immediate: case Operation::or_immediate: case Operation::xor_immediate:
            return instruction.operand;
        case Operation::add: case Operation::subtract: case Operation::add_checked: case Operation::subtract_checked:
        case Operation::bit_and: case Operation::bit_or: case Operation::bit_xor: case Operation::bit_nor:
        case Operation::less_signed: case Operation::less_unsigned:
        case Operation::shift_left: case Operation::shift_right: case Operation::shift_arithmetic:
        case Operation::shift_left_variable: case Operation::shift_right_variable: case Operation::shift_arithmetic_variable:
        case Operation::read_hi: case Operation::read_lo: case Operation::call_register:
            return instruction.destination;
        case Operation::call: return 31;
        default: return 0;
        }
    };
    const auto is_load=[](Operation operation) {
        return operation==Operation::load_byte || operation==Operation::load_half || operation==Operation::load_word ||
            operation==Operation::load_byte_unsigned || operation==Operation::load_half_unsigned ||
            operation==Operation::load_word_left || operation==Operation::load_word_right;
    };
    const auto clobbers=[&](const Instruction& instruction) {
        return instruction.operation==Operation::syscall || instruction.operation==Operation::native_import ||
            instruction.operation==Operation::memory_fill || instruction.operation==Operation::memory_copy ||
            instruction.operation==Operation::memory_move;
    };
    const auto apply=[&](Id in,const Instruction& instruction,const Constants& constants) {
        if (clobbers(instruction)) return Id{};
        Id out; out.regs=in.regs;
        const unsigned load_reg=is_load(instruction.operation) ? instruction.operand : 0;
        const unsigned written=load_reg ? 0 : written_register(instruction);
        for (unsigned reg=1;reg<32;++reg) {
            if (in.delayed[reg].kind==Cell::Kind::none || reg==written || reg==load_reg) continue;
            out.regs[reg]=in.delayed[reg];
        }
        if (written) out.regs[written]=Cell::unknown_cell();
        if (load_reg) {
            Cell value=Cell::unknown_cell();
            if (instruction.operation==Operation::load_word)
                if (const auto address=one_address(constants,instruction.source,instruction.immediate); address && *address%4==0)
                    value=Cell::slot_of(*address);
            out.delayed[load_reg]=value;
        }
        return out;
    };
    const auto has_delay=[](Flow flow) {
        return flow==Flow::direct || flow==Flow::conditional || flow==Flow::call || flow==Flow::indirect ||
            flow==Flow::indirect_call || flow==Flow::return_register;
    };
    struct Edge { std::uint32_t to=0; bool returns=false; };
    std::map<std::uint32_t,std::vector<Edge>> edges;
    std::map<std::uint32_t,unsigned> predecessors;
    for (const auto& [pc,instruction] : program.instructions) {
        if (!states.contains(pc)) continue;
        predecessors.emplace(pc,0);
        const auto add=[&](std::uint32_t target,bool returns) {
            if (!states.contains(target)) return;
            edges[pc].push_back({target,returns}); ++predecessors[target];
        };
        switch (instruction.flow) {
        case Flow::sequential: case Flow::service: add(pc+4,false); break;
        case Flow::direct: add(instruction.target,false); break;
        case Flow::conditional: add(instruction.target,false); add(pc+8,false); break;
        case Flow::call: add(instruction.target,false); add(pc+8,true); break;
        case Flow::indirect: for (const auto target : instruction.targets) add(target,false); break;
        case Flow::indirect_call:
            for (const auto target : instruction.targets) add(target,false);
            add(pc+8,true); break;
        default: break;
        }
    }
    std::map<std::uint32_t,Id> known;
    std::vector<std::uint32_t> work;
    const auto relax=[&](std::uint32_t pc,const Id& incoming) {
        const auto found=known.find(pc);
        if (found==known.end()) { known.emplace(pc,incoming); work.push_back(pc); return; }
        const auto merged=merge_id(found->second,incoming);
        if (merged==found->second) return;
        found->second=merged; work.push_back(pc);
    };
    for (const auto& [pc,count] : predecessors) if (!count) relax(pc,Id{});
    while (!work.empty()) {
        const auto pc=work.back(); work.pop_back();
        const auto& instruction=program.instructions.at(pc);
        for (const auto edge : edges[pc]) {
            Id out{};
            if (!edge.returns) {
                out=apply(known.at(pc),instruction,states.at(pc));
                if (has_delay(instruction.flow)) if (const auto delay=program.instructions.find(pc+4); delay!=program.instructions.end())
                    out=apply(out,delay->second,transfer(instruction,states.at(pc)));
            }
            relax(edge.to,out);
        }
    }
    std::map<std::uint32_t,std::set<std::uint32_t>> recovered;
    for (const auto& [pc,instruction] : program.instructions) {
        if (instruction.flow!=Flow::indirect && instruction.flow!=Flow::indirect_call) continue;
        const auto state=states.find(pc);
        const auto identity=known.find(pc);
        if (state==states.end() || identity==known.end() || state->second.registers[instruction.source]) continue;
        const auto& cell=identity->second.regs[instruction.source];
        const auto owner=cell.kind==Cell::Kind::slot ? module_of(cell.slot) : std::optional<std::size_t>{};
        if (cell.kind!=Cell::Kind::slot || !owner || module_wild[*owner] || poisoned.contains(cell.slot)) continue;
        std::set<std::uint32_t> targets=stored[cell.slot];
        const auto initial=image_word(cell.slot);
        if (!initial || (*initial && !add_code(*initial,targets)) || targets.empty() || targets.size()>value_limit) continue;
        recovered.emplace(pc,std::move(targets));
    }
    return recovered;
}
}
