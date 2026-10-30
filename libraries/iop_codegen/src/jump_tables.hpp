#pragma once
#include <nt/iop_ir.hpp>
#include <optional>
#include <array>
#include <stdexcept>

namespace nt::iop_codegen::ir {
// Initial table contents supply bounded candidates, not immutable-memory proofs.
// Generated indirect transfers still check their actual runtime destination.
template<class Decode>
std::optional<std::set<std::uint32_t>> initial_jump_table(const elf::IopModule& module,
    std::span<const elf::IopModule> data,std::uint32_t pc,Decode decode) {
    if (pc<module.base || pc-module.base<28 || pc-module.base+4>module.initialized_size) return {};
    std::array<Instruction,8> code;
    Instruction guard,branch;
    try {
        for (unsigned i=0;i<code.size();++i) code[i]=decode(pc-28+i*4,le32(module.memory,pc-module.base-28+i*4));
        if (code[0].operation==Operation::less_immediate_unsigned) {
            guard=code[0]; branch=code[1]; // Index shift is the branch's delay slot.
        } else {
            if (pc-module.base<32) return {};
            guard=decode(pc-32,le32(module.memory,pc-module.base-32));
            branch=code[0];
            if (code[1].flow!=Flow::sequential) return {};
        }
    } catch (const std::runtime_error&) { return {}; }
    const auto& shift=code[2]; const auto& upper=code[3]; const auto& add=code[4];
    const auto& load=code[5]; const auto& jump=code[7];
    if (guard.operation!=Operation::less_immediate_unsigned || !guard.immediate || guard.immediate>256 ||
        branch.operation!=Operation::equal || branch.source!=guard.operand || branch.operand!=0 ||
        (branch.target>=pc-20 && branch.target<=pc+4) ||
        shift.operation!=Operation::shift_left || shift.shift!=2 || shift.operand!=guard.source ||
        upper.operation!=Operation::upper_immediate || add.operation!=Operation::add ||
        !((add.source==upper.operand && add.operand==shift.destination) ||
          (add.operand==upper.operand && add.source==shift.destination)) ||
        load.operation!=Operation::load_word || load.source!=add.destination ||
        (jump.flow!=Flow::indirect && jump.flow!=Flow::indirect_call) || jump.source!=load.operand ||
        le32(module.memory,pc-module.base-4)!=0) return {};
    const auto raw=(upper.immediate<<16)+load.immediate;
    const auto segment=raw&0xe0000000U;
    const auto base=(segment==0x80000000U || segment==0xa0000000U) ? raw&0x1fffffffU : raw;
    if (base%4) return {};
    for (const auto& owner : data) {
        if (base<owner.base) continue;
        const auto offset=base-owner.base,bytes=guard.immediate*4;
        if (offset>owner.initialized_size || bytes>owner.initialized_size-offset ||
            offset>owner.memory.size() || bytes>owner.memory.size()-offset) continue;
        std::set<std::uint32_t> targets;
        for (unsigned i=0;i<guard.immediate;++i) {
            auto target=le32(owner.memory,offset+i*4);
            const auto region=target&0xe0000000U;
            if (region==0x80000000U || region==0xa0000000U) target&=0x1fffffffU;
            if (!target || target%4) return {};
            targets.insert(target);
        }
        return targets;
    }
    return {};
}
}
