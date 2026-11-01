#pragma once
#include "constants.hpp"
#include <algorithm>
#include <stdexcept>

namespace nt::iop_codegen::ir {
// An unrolled suffix can select its first instruction as end - index * 4.
// Discover only the adjacent straight-line tail ending at that literal return.
// These are candidates, not a range proof for index: generated code must reject
// every runtime destination outside this bounded set before its delay slot.
template<class Decode>
std::optional<std::set<std::uint32_t>> inline_tail_targets(const elf::IopModule& module,
    const Program& program,const std::map<std::uint32_t,Constants>& states,
    const Instruction& jump,Decode decode) {
    if (jump.flow!=Flow::indirect || jump.address<module.base ||
        std::uint64_t(jump.address)+8>0xffffffffU) return {};
    for (unsigned distance=4;distance<=24 && distance+4<=jump.address-module.base;distance+=4) {
        const auto at=jump.address-distance;
        const auto producer=program.instructions.find(at),shift=program.instructions.find(at-4);
        const auto input=states.find(at);
        if (producer==program.instructions.end() || shift==program.instructions.end() || input==states.end()) continue;
        const auto& subtract=producer->second;
        if ((subtract.operation!=Operation::subtract && subtract.operation!=Operation::subtract_checked) ||
            !subtract.destination || subtract.source==subtract.operand ||
            shift->second.operation!=Operation::shift_left || shift->second.shift!=2 ||
            shift->second.destination!=subtract.operand) continue;
        const auto& base=input->second.registers[subtract.source];
        if (!base || base->size()!=1) continue;
        auto end=*base->begin();
        const auto segment=end&0xe0000000U;
        if (segment==0x80000000U || segment==0xa0000000U) end&=0x1fffffffU;
        const auto first=jump.address+8;
        const auto extent=std::min<std::size_t>(module.initialized_size,module.memory.size());
        if (end%4 || end<first || end-first>60 || std::uint64_t(end-module.base)+8>extent) continue;
        Constants marker; marker.registers[subtract.destination]=constant(0x6d800001);
        bool preserved=true;
        for (auto pc=at+4;pc<jump.address;pc+=4) {
            const auto middle=program.instructions.find(pc);
            if (middle==program.instructions.end() || middle->second.flow!=Flow::sequential) { preserved=false; break; }
            marker=transfer(middle->second,std::move(marker));
        }
        if (!preserved || marker.registers[jump.source]!=constant(0x6d800001)) continue;
        std::set<std::uint32_t> targets;
        try {
            for (auto pc=first;pc<=end+4;pc+=4) {
                const auto instruction=decode(pc,le32(module.memory,pc-module.base));
                if (pc==end ? instruction.flow!=Flow::return_register : instruction.flow!=Flow::sequential) {
                    targets.clear(); break;
                }
                if (pc<=end) targets.insert(pc);
            }
        } catch (const std::runtime_error&) { targets.clear(); }
        if (!targets.empty()) return targets;
    }
    return {};
}
}
