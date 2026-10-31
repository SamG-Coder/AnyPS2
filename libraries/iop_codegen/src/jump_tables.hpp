#pragma once
#include <nt/iop_ir.hpp>
#include "constants.hpp"
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
            // A stack spill between the comparison and its branch preserves
            // both the tested index and predicate registers.
            for (unsigned distance=36;guard.operation==Operation::store_word && guard.source==29 &&
                    distance<=48 && pc-module.base>=distance;distance+=4)
                guard=decode(pc-distance,le32(module.memory,pc-module.base-distance));
        }
    } catch (const std::runtime_error&) { return {}; }
    // A loop may advance a table cursor in parallel with its checked index.
    // Recover the initial base only when the latch increments that index by one
    // and the pointer by one word. The emitted transfer still guards the value.
    if (code[2].operation==Operation::less_immediate_unsigned && code[2].immediate && code[2].immediate<=256 &&
        code[3].operation==Operation::equal && code[3].source==code[2].operand && code[3].operand==0 &&
        (code[3].target<pc-8 || code[3].target>pc+4) && code[4].flow==Flow::sequential &&
        code[5].operation==Operation::load_word && code[5].source>=16 && code[5].source<=23 &&
        code[7].flow==Flow::indirect && code[7].source==code[5].operand && le32(module.memory,pc-module.base-4)==0) {
        const auto cursor=code[5].source,index=code[2].source;
        Constants marker; marker.registers[cursor]=constant(0x6d800001); marker.registers[index]=constant(0x6d800002);
        marker=transfer(code[4],std::move(marker));
        if (cursor!=index && marker.registers[cursor]==constant(0x6d800001) && marker.registers[index]==constant(0x6d800002)) {
            const auto first=pc-module.base>128 ? pc-128 : module.base;
            for (auto at=first;at+8<=pc-20;at+=4) {
                Instruction upper,lower;
                try { upper=decode(at,le32(module.memory,at-module.base)); lower=decode(at+4,le32(module.memory,at+4-module.base)); }
                catch (const std::runtime_error&) { continue; }
                if (upper.operation!=Operation::upper_immediate || upper.operand!=cursor ||
                    lower.operation!=Operation::add_immediate || lower.source!=cursor || lower.operand!=cursor) continue;
                for (auto latch=pc+8;latch<=pc+512 && latch-module.base+16<=module.initialized_size;latch+=4) {
                    std::array<Instruction,4> loop;
                    try { for (unsigned i=0;i<4;++i) loop[i]=decode(latch+i*4,le32(module.memory,latch+i*4-module.base)); }
                    catch (const std::runtime_error&) { continue; }
                    if (loop[0].operation!=Operation::add_immediate || loop[0].source!=index || loop[0].operand!=index || loop[0].immediate!=1 ||
                        (loop[1].operation!=Operation::less_immediate_signed && loop[1].operation!=Operation::less_immediate_unsigned) || loop[1].source!=index ||
                        loop[2].operation!=Operation::not_equal || loop[2].source!=loop[1].operand || loop[2].operand!=0 ||
                        loop[2].target<at+8 || loop[2].target>pc-20 ||
                        loop[3].operation!=Operation::add_immediate || loop[3].source!=cursor || loop[3].operand!=cursor || loop[3].immediate!=4) continue;
                    if (const auto targets=table_targets(data,(upper.immediate<<16)+lower.immediate+code[5].immediate,code[2].immediate)) return targets;
                }
            }
        }
    }
    // Some compilers form table[index] in a saved register before the range
    // check. Require the complete address expression and a straight-line,
    // non-clobbering path to the guard; initial entries remain only candidates.
    if (code[2].operation==Operation::less_immediate_unsigned && code[2].immediate && code[2].immediate<=256 &&
        code[3].operation==Operation::equal && code[3].source==code[2].operand && code[3].operand==0 &&
        (code[3].target<pc-8 || code[3].target>pc+4) && le32(module.memory,pc-module.base-12)==0 &&
        code[5].operation==Operation::load_word && (code[7].flow==Flow::indirect || code[7].flow==Flow::indirect_call) &&
        code[7].source==code[5].operand && le32(module.memory,pc-module.base-4)==0) {
        const auto first=pc-module.base>128 ? pc-128 : module.base;
        for (auto at=first;at+16<=pc-20;at+=4) {
            std::array<Instruction,4> address;
            try { for (unsigned i=0;i<4;++i) address[i]=decode(at+i*4,le32(module.memory,at+i*4-module.base)); }
            catch (const std::runtime_error&) { continue; }
            const auto& upper=address[0]; const auto& lower=address[1]; const auto& shift=address[2]; const auto& add=address[3];
            if (upper.operation!=Operation::upper_immediate || lower.operation!=Operation::add_immediate ||
                lower.source!=upper.operand || lower.operand!=upper.operand ||
                shift.operation!=Operation::shift_left || shift.shift!=2 || shift.operand!=code[2].source ||
                add.operation!=Operation::add || add.destination!=code[5].source ||
                !((add.source==shift.destination && add.operand==lower.operand) ||
                  (add.operand==shift.destination && add.source==lower.operand))) continue;
            Constants marker; marker.registers[add.destination]=constant(0x6d800001);
            marker.registers[code[2].source]=constant(0x6d800002);
            bool preserved=add.destination!=code[2].source;
            for (auto step=at+16;preserved && step<pc-20;step+=4) {
                try {
                    const auto instruction=decode(step,le32(module.memory,step-module.base));
                    if (instruction.flow!=Flow::sequential) { preserved=false; break; }
                    marker=transfer(instruction,std::move(marker));
                } catch (const std::runtime_error&) { preserved=false; }
            }
            if (preserved && marker.registers[add.destination]==constant(0x6d800001) &&
                marker.registers[code[2].source]==constant(0x6d800002))
                if (const auto targets=table_targets(data,(upper.immediate<<16)+lower.immediate+code[5].immediate,code[2].immediate))
                    return targets;
        }
    }
    // Hoisted saved-register bases are address candidates, not proof that the
    // register or initial table stays immutable. Runtime target guards remain.
    const auto guard_index=code[0].operation==Operation::less_immediate_unsigned ? 0U : 1U;
    const auto& hoisted_guard=code[guard_index];
    const auto& hoisted_branch=code[guard_index+1];
    bool preserved_index=true;
    if (!guard_index) {
        Constants marker; marker.registers[hoisted_guard.source]=constant(0x6d800001);
        marker=transfer(code[2],std::move(marker));
        preserved_index=code[2].flow==Flow::sequential && marker.registers[hoisted_guard.source]==constant(0x6d800001);
    }
    if (hoisted_guard.operation==Operation::less_immediate_unsigned && preserved_index &&
        hoisted_branch.operation==Operation::equal && hoisted_branch.source==hoisted_guard.operand && hoisted_branch.operand==0 &&
        (hoisted_branch.target<pc-16 || hoisted_branch.target>pc+4) &&
        code[3].operation==Operation::shift_left && code[3].shift==2 && code[3].operand==hoisted_guard.source &&
        code[4].operation==Operation::add && code[5].operation==Operation::load_word &&
        code[5].source==code[4].destination &&
        (code[7].flow==Flow::indirect || code[7].flow==Flow::indirect_call) && code[7].source==code[5].operand &&
        le32(module.memory,pc-module.base-4)==0) {
        const auto& add=code[4];
        const auto index=code[3].destination;
        const auto base_register=add.source==index ? add.operand : add.operand==index ? add.source : 0;
        if (base_register && base_register!=29 && hoisted_guard.immediate && hoisted_guard.immediate<=256) {
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
                if (const auto targets=table_targets(data,(upper.immediate<<16)+lower.immediate+code[5].immediate,hoisted_guard.immediate))
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
