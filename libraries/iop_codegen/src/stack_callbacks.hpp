#pragma once
#include "constants.hpp"

namespace nt::iop_codegen::ir {
// A stack-loaded function pointer may cross helper calls that erase constant
// proofs. Recover only a bounded set of addresses actually stored or passed by
// this module. This is candidate discovery; the emitted transfer must guard the
// actual pointer and reject any destination outside the compiled set.
inline bool stack_callback_call(const Program& program,const Instruction& call) {
    if (call.flow!=Flow::indirect_call) return false;
    for (unsigned distance=8;distance<=128 && distance<=call.address;distance+=4) {
        const auto found=program.instructions.find(call.address-distance);
        if (found==program.instructions.end()) continue;
        const auto& load=found->second;
        if (load.operation!=Operation::load_word || load.source!=29 || load.operand!=call.source) continue;
        Constants marker; marker.registers[call.source]=constant(1);
        bool valid=true;
        for (auto pc=load.address+4;pc<call.address;pc+=4) {
            const auto at=program.instructions.find(pc);
            if (at==program.instructions.end() ||
                (at->second.flow!=Flow::sequential && at->second.flow!=Flow::conditional)) { valid=false; break; }
            marker=transfer(at->second,std::move(marker));
        }
        if (valid && marker.registers[call.source]==constant(1)) return true;
    }
    return false;
}
// An indirect call fed by a word load may use a dynamically populated registry.
// Relocation candidates retain possible callees without proving the loaded value.
inline bool loaded_callback_call(const Program& program,const Instruction& call) {
    if (call.flow!=Flow::indirect_call) return false;
    for (unsigned distance=8;distance<=48 && distance<=call.address;distance+=4) {
        const auto found=program.instructions.find(call.address-distance);
        if (found==program.instructions.end()) continue;
        const auto& load=found->second;
        if (load.operation!=Operation::load_word || load.operand!=call.source || load.source==29) continue;
        const auto indexed=program.instructions.find(load.address-4);
        if (indexed==program.instructions.end() || indexed->second.operation!=Operation::add ||
            indexed->second.destination!=load.source || !indexed->second.source || !indexed->second.operand) continue;
        Constants marker; marker.registers[call.source]=constant(1);
        bool valid=true;
        for (auto pc=load.address+4;pc<call.address;pc+=4) {
            const auto at=program.instructions.find(pc);
            if (at==program.instructions.end() ||
                (at->second.flow!=Flow::sequential && at->second.flow!=Flow::conditional)) { valid=false; break; }
            marker=transfer(at->second,std::move(marker));
        }
        if (valid && marker.registers[call.source]==constant(1)) return true;
    }
    return false;
}
inline std::set<std::uint32_t> stored_code_candidates(const Program& program,
    const std::map<std::uint32_t,Constants>& states,std::span<const elf::IopModule> modules,bool include_arguments=true) {
    std::set<std::uint32_t> result;
    const auto add=[&](const Constant& values) {
        if (!values) return;
        for (auto value : *values) {
            const auto segment=value&0xe0000000U;
            if (segment==0x80000000U || segment==0xa0000000U) value&=0x1fffffffU;
            if (!value || value%4) continue;
            for (const auto& owner : modules) {
                const auto size=owner.info ? owner.info->text_size : owner.initialized_size;
                if (value>=owner.base && value-owner.base<size) { result.insert(value); break; }
            }
        }
    };
    for (const auto& [pc,instruction] : program.instructions) {
        const auto state=states.find(pc);
        if (state==states.end()) continue; // Delay-slot-only instructions have no entry state.
        if (instruction.operation==Operation::store_word) add(state->second.registers[instruction.operand]);
        if (instruction.flow==Flow::call || instruction.flow==Flow::indirect_call) {
            auto arguments=transfer(instruction,state->second);
            if (const auto delay=program.instructions.find(pc+4);delay!=program.instructions.end()) {
                if (delay->second.operation==Operation::store_word) add(arguments.registers[delay->second.operand]);
                arguments=transfer(delay->second,std::move(arguments));
            }
            if (include_arguments) for (unsigned reg=4;reg<=7;++reg) add(arguments.registers[reg]);
        }
        if (result.size()>256) return {};
    }
    return result;
}
}
