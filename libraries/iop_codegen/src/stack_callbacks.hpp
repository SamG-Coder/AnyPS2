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
inline bool indexed_load_source(const Program& program,const Instruction& load,unsigned depth=0) {
    if (depth>=3 || load.source==29) return false;
    for (unsigned distance=4;distance<=24 && distance<=load.address;distance+=4) {
        const auto at=program.instructions.find(load.address-distance);
        if (at==program.instructions.end()) continue;
        const auto& producer=at->second;
        const bool indexed=producer.operation==Operation::add && producer.destination==load.source &&
            producer.source && producer.operand;
        const bool nested=producer.operation==Operation::load_word && producer.operand==load.source;
        if (!indexed && !nested) continue;
        Constants marker; marker.registers[load.source]=constant(0x6d800001);
        bool preserved=true;
        for (auto pc=producer.address+4;pc<load.address;pc+=4) {
            const auto middle=program.instructions.find(pc);
            if (middle==program.instructions.end() || middle->second.flow!=Flow::sequential) { preserved=false; break; }
            marker=transfer(middle->second,std::move(marker));
        }
        if (preserved && marker.registers[load.source]==constant(0x6d800001) &&
            (indexed || indexed_load_source(program,producer,depth+1))) return true;
    }
    return false;
}
inline bool indexed_load_reaches(const Program& program,std::uint32_t address,unsigned reg,unsigned minimum_distance=8) {
    for (unsigned distance=minimum_distance;distance<=48 && distance<=address;distance+=4) {
        const auto found=program.instructions.find(address-distance);
        if (found==program.instructions.end()) continue;
        const auto& load=found->second;
        if (load.operation!=Operation::load_word || load.source==29) continue;
        if (!indexed_load_source(program,load)) continue;
        Constants marker; marker.registers[load.operand]=constant(0x6d800001);
        bool valid=true;
        for (auto pc=load.address+4;pc<address;pc+=4) {
            const auto at=program.instructions.find(pc);
            if (at==program.instructions.end() ||
                (at->second.flow!=Flow::sequential && at->second.flow!=Flow::conditional)) { valid=false; break; }
            marker=transfer(at->second,std::move(marker));
        }
        if (valid && marker.registers[reg]==constant(0x6d800001)) return true;
    }
    return false;
}
inline bool loaded_callback_call(const Program& program,const Instruction& call) {
    return call.flow==Flow::indirect_call && indexed_load_reaches(program,call.address,call.source);
}
// A registry helper may return an indexed function-pointer load to its caller.
// This recognizes a candidate source, not an immutable table or a value proof.
// Every emitted transfer must still reject destinations outside compiled code.
inline bool returned_callback_call(const Program& program,const Instruction& call) {
    if (call.flow!=Flow::indirect_call) return false;
    for (unsigned distance=8;distance<=128 && distance<=call.address;distance+=4) {
        const auto helper=program.instructions.find(call.address-distance);
        if (helper==program.instructions.end() || helper->second.flow!=Flow::call) continue;
        Constants marker; marker.registers[2]=constant(0x6d800001);
        bool preserved=true;
        for (auto pc=helper->first+8;pc<call.address;pc+=4) {
            const auto at=program.instructions.find(pc);
            if (at==program.instructions.end() || (at->second.flow!=Flow::sequential && at->second.flow!=Flow::conditional)) {
                preserved=false; break;
            }
            marker=transfer(at->second,std::move(marker));
        }
        if (!preserved || marker.registers[call.source]!=constant(0x6d800001)) continue;
        std::vector<std::uint32_t> pending{helper->second.target};
        std::set<std::uint32_t> seen;
        while (!pending.empty() && seen.size()<256) {
            const auto pc=pending.back(); pending.pop_back();
            if (!seen.insert(pc).second) continue;
            const auto at=program.instructions.find(pc);
            if (at==program.instructions.end()) continue;
            const auto& i=at->second;
            if (i.flow==Flow::return_register && indexed_load_reaches(program,pc,2,4)) return true;
            if (i.flow==Flow::sequential) pending.push_back(pc+4);
            if (i.flow==Flow::direct || i.flow==Flow::conditional) pending.push_back(i.target);
            if (i.flow==Flow::conditional) pending.push_back(pc+8);
        }
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
