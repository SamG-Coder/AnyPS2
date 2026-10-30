#pragma once
#include <nt/iop_ir.hpp>
#include <optional>
#include <array>
#include <stdexcept>

namespace nt::iop_codegen::ir {
inline std::optional<std::set<std::uint32_t>> table_targets(std::span<const elf::IopModule> data, std::uint32_t raw, std::uint32_t count) {
    const auto segment=raw&0xe0000000U;
    const auto base=(segment==0x80000000U || segment==0xa0000000U) ? raw&0x1fffffffU : raw;
    if (base%4) return {};
    for (const auto& owner : data) {
        if (base<owner.base) continue;
        const auto offset=base-owner.base,bytes=count*4;
        if (offset>owner.initialized_size || bytes>owner.initialized_size-offset ||
            offset>owner.memory.size() || bytes>owner.memory.size()-offset) continue;
        std::set<std::uint32_t> targets;
        for (unsigned i=0;i<count;++i) {
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
    // Hoisted saved-register bases are address candidates, not proof that the
    // register or initial table stays immutable. Runtime target guards remain.
    if (code[1].operation==Operation::less_immediate_unsigned &&
        code[2].operation==Operation::equal && code[2].source==code[1].operand && code[2].operand==0 &&
        (code[2].target<pc-16 || code[2].target>pc+4) &&
        code[3].operation==Operation::shift_left && code[3].shift==2 && code[3].operand==code[1].source &&
        code[4].operation==Operation::add && code[5].operation==Operation::load_word &&
        code[5].source==code[4].destination &&
        (code[7].flow==Flow::indirect || code[7].flow==Flow::indirect_call) && code[7].source==code[5].operand &&
        le32(module.memory,pc-module.base-4)==0) {
        const auto& add=code[4];
        const auto index=code[3].destination;
        const auto base_register=add.source==index ? add.operand : add.operand==index ? add.source : 0;
        if (base_register>=16 && base_register<=23 && code[1].immediate && code[1].immediate<=256) {
            std::set<std::uint32_t> candidates;
            const auto first=pc-module.base>512 ? pc-512 : module.base;
            for (auto at=first;at+4<pc-24;at+=4) {
                Instruction upper,lower;
                try {
                    upper=decode(at,le32(module.memory,at-module.base));
                    lower=decode(at+4,le32(module.memory,at+4-module.base));
                } catch (const std::runtime_error&) { continue; }
                if (upper.operation!=Operation::upper_immediate || upper.operand!=base_register ||
                    lower.operation!=Operation::add_immediate || lower.source!=base_register || lower.operand!=base_register) continue;
                if (const auto targets=table_targets(data,(upper.immediate<<16)+lower.immediate+code[5].immediate,code[1].immediate))
                    candidates.insert(targets->begin(),targets->end());
                if (candidates.size()>256) return {};
            }
            if (!candidates.empty()) return candidates;
        }
    }
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
    return table_targets(data,(upper.immediate<<16)+load.immediate,guard.immediate);
}
}
