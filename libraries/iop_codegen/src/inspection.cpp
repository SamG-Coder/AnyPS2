#include <nt/iop_inspection.hpp>
#include "constants.hpp"
#include <algorithm>
#include <stdexcept>

namespace nt::iop_codegen::ir {
Neighborhood inspect_neighborhood(const elf::IopModule& module,std::uint32_t anchor,std::uint32_t radius) {
    const auto size=std::min<std::size_t>(module.initialized_size,module.memory.size());
    const auto extent=module.info ? std::min<std::size_t>(size,module.info->text_size) : size;
    if (!radius || radius>0x100000 || module.base>0x200000 || extent>0x200000-module.base ||
        anchor%4 || anchor<module.base || anchor-module.base>=extent)
        throw std::invalid_argument("Invalid IOP inspection anchor, radius or module extent");
    Neighborhood result; result.anchor=anchor;
    result.first=std::max(module.base,anchor>radius ? anchor-radius : 0U)&~3U;
    result.last=static_cast<std::uint32_t>(std::min<std::uint64_t>(module.base+extent,std::uint64_t(anchor)+radius+4))&~3U;
    std::map<std::uint32_t,elf::IopImport> imports;
    for (auto item:elf::iop_imports(module)) {
        item.offset+=module.base; item.table_offset+=module.base; imports.emplace(item.offset,std::move(item));
    }
    const auto code_candidate=[&](std::uint32_t value) {
        return !(value%4) && value>=module.base && value-module.base<extent && !imports.contains(value);
    };
    Constants state;
    std::map<std::int32_t,std::uint8_t> stack_bytes;
    const auto track_store=[&](const Instruction& instruction,const Constants& before) {
        const auto op=instruction.operation;
        if (op!=Operation::store_byte && op!=Operation::store_half && op!=Operation::store_word &&
            op!=Operation::store_word_left && op!=Operation::store_word_right) return;
        if (!before.stack_offsets[instruction.source] || op==Operation::store_word_left || op==Operation::store_word_right) {
            stack_bytes.clear(); return;
        }
        const auto start=std::int64_t(*before.stack_offsets[instruction.source])+static_cast<std::int32_t>(instruction.immediate);
        if (start<-65536 || start>65532) { stack_bytes.clear(); return; }
        const unsigned width=op==Operation::store_byte ? 1 : op==Operation::store_half ? 2 : 4;
        for (unsigned index=0;index<width;++index) {
            const auto at=static_cast<std::int32_t>(start+index);
            stack_bytes.erase(at);
            const auto& value=before.registers[instruction.operand];
            if (value && value->size()==1) stack_bytes[at]=static_cast<std::uint8_t>(*value->begin()>>(8*index));
        }
    };
    for (auto pc=result.first;pc<result.last;pc+=4) {
        Instruction instruction;
        try { instruction=decode_instruction(pc,le32(module.memory,pc-module.base)); }
        catch (const std::exception& error) { result.unknown.emplace(pc,error.what()); state=Constants{}; stack_bytes.clear(); continue; }
        if (instruction.operation==Operation::add_immediate && instruction.source==29 && instruction.operand==29 &&
            static_cast<std::int32_t>(instruction.immediate)<0) result.function_candidates.insert(pc);
        if (instruction.width) ++result.patterns["memory-width-"+std::to_string(instruction.width)];
        if (instruction.flow==Flow::conditional) ++result.patterns["conditional-branch"];
        if (instruction.flow==Flow::indirect || instruction.flow==Flow::indirect_call) ++result.patterns["indirect-control-flow"];
        track_store(instruction,state);
        auto after=transfer(instruction,state);
        const bool call=instruction.flow==Flow::call || instruction.flow==Flow::indirect_call ||
            (instruction.flow==Flow::direct && imports.contains(instruction.target));
        const bool delayed=instruction.flow==Flow::call || instruction.flow==Flow::indirect_call ||
            instruction.flow==Flow::direct || instruction.flow==Flow::indirect ||
            instruction.flow==Flow::conditional || instruction.flow==Flow::return_register;
        bool valid_delay=true;
        if (delayed) {
            if (pc+4>=result.last) valid_delay=false;
            else try {
                const auto delay=decode_instruction(pc+4,le32(module.memory,pc+4-module.base));
                if (delay.flow!=Flow::sequential) valid_delay=false;
                else { track_store(delay,after); after=transfer(delay,after); }
            } catch (const std::exception& error) { result.unknown.emplace(pc+4,error.what()); valid_delay=false; }
        }
        if (call) {
            CallPattern site; site.address=pc; site.target=instruction.target; site.indirect=instruction.flow==Flow::indirect_call;
            if (const auto found=imports.find(site.target);found!=imports.end()) site.import=found->second;
            else if (code_candidate(site.target)) result.function_candidates.insert(site.target);
            if (valid_delay) {
                for (unsigned reg=0;reg<4;++reg) site.arguments[reg]=after.registers[4+reg];
                for (unsigned reg=0;reg<4;++reg) if (after.stack_offsets[4+reg]) {
                    std::array<std::optional<std::uint8_t>,16> bytes{};
                    bool any=false;
                    for (unsigned index=0;index<bytes.size();++index) {
                        const auto at=std::int64_t(*after.stack_offsets[4+reg])+index;
                        if (at<std::numeric_limits<std::int32_t>::min() || at>std::numeric_limits<std::int32_t>::max()) continue;
                        const auto found=stack_bytes.find(static_cast<std::int32_t>(at));
                        if (found!=stack_bytes.end()) { bytes[index]=found->second; any=true; }
                    }
                    if (any) site.stack_arguments.emplace(reg,bytes);
                }
                if (after.stack_offsets[29]) {
                    for (const auto& [offset,value]:after.stack_words) if (value) {
                        const auto relative=std::int64_t(offset)-*after.stack_offsets[29];
                        if (relative>=0 && relative<=256) site.stack_words.emplace(static_cast<std::int32_t>(relative),*value);
                    }
                    for (unsigned argument=4;argument<8;++argument)
                        if (const auto value=site.stack_words.find(argument*4);value!=site.stack_words.end()) site.arguments[argument]=value->second;
                }
            }
            for (const auto& argument:site.arguments) if (argument)
                for (const auto value:*argument) if (code_candidate(value)) site.code_candidates.insert(value);
            result.calls.push_back(std::move(site));
        }
        if (delayed) { state=Constants{}; stack_bytes.clear(); if (pc+4<result.last) pc+=4; }
        else if (instruction.flow==Flow::sequential) state=std::move(after);
        else state=Constants{};
    }
    return result;
}
}
