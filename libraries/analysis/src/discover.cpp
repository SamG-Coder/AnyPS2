#include <nt/analysis.hpp>
#include "pointer_flow.hpp"
#include <algorithm>
#include <deque>
#include <optional>
#include <stdexcept>

namespace nt::analysis {
Program whole_code(ByteView bytes,const elf::Executable& executable,std::size_t limit) {
    if (executable.inferred_permissions || executable.code_sections.empty())
        throw std::runtime_error("Whole-code conversion requires declared EE instruction sections; classify code/data for this image first");
    Program result;
    result.entries.insert(executable.entry);
    result.entries.insert(executable.function_entries.begin(),executable.function_entries.end());
    result.entries.insert(executable.lifecycle_entries.begin(),executable.lifecycle_entries.end());
    for (const auto& range:executable.code_sections) {
        if (!range.size || range.address%4 || range.size%4 ||
            std::uint64_t(range.address)+range.size>0x100000000ULL)
            throw std::runtime_error("Invalid whole-code instruction extent");
        const auto segment=std::find_if(executable.segments.begin(),executable.segments.end(),[&](const auto& s) {
            return (s.flags&1) && range.address>=s.address &&
                std::uint64_t(range.address)+range.size<=std::uint64_t(s.address)+s.file_size;
        });
        if (segment==executable.segments.end()) throw std::runtime_error("Whole-code section has no file-backed executable segment");
        const auto offset=std::uint64_t(segment->offset)+range.address-segment->address;
        require_range(bytes,static_cast<std::size_t>(offset),range.size);
        for (std::uint32_t n=0;n<range.size;n+=4) {
            const auto pc=range.address+n;
            if (!elf::executable_address(executable,pc))
                throw std::runtime_error("Whole-code section overlaps non-EE contents at "+hex32(pc));
            if (!result.instructions.emplace(pc,r5900::decode(pc,le32(bytes,static_cast<std::size_t>(offset+n)))).second)
                throw std::runtime_error("Overlapping whole-code sections at "+hex32(pc));
            result.ordinary.insert(pc);
            if (result.instructions.size()>limit) throw std::runtime_error("Whole-code instruction limit exceeded");
        }
    }
    for (const auto entry:result.entries)
        if (!result.instructions.contains(entry)) result.unresolved.insert(entry);
    for (const auto& [pc,instruction]:result.instructions) {
        if (!instruction.branch()) continue;
        if (!result.instructions.contains(pc+4)) result.unresolved.insert(pc+4);
        if (!instruction.indirect() && !result.instructions.contains(instruction.target()))
            result.unresolved.insert(instruction.target());
    }
    return result;
}
std::set<std::uint32_t> syscall_wrappers(ByteView bytes,const elf::Executable& executable) {
    std::set<std::uint32_t> result;
    for (const auto& segment:executable.segments) {
        if (!(segment.flags&1)) continue;
        if (std::uint64_t(segment.offset)+segment.file_size>bytes.size() ||
            std::uint64_t(segment.address)+segment.file_size>0x100000000ULL)
            throw std::invalid_argument("Invalid syscall-wrapper scan extent");
        const auto first=(4U-(segment.address&3U))&3U;
        for (std::uint64_t offset=first;offset+16<=segment.file_size;offset+=4) {
            const auto pc=segment.address+static_cast<std::uint32_t>(offset);
            if (!elf::executable_address(executable,pc,16)) continue;
            const auto at=static_cast<std::size_t>(segment.offset+offset);
            const auto setup=r5900::decode(pc,le32(bytes,at));
            if (setup.rs || setup.rt!=3 ||
                (setup.op!=r5900::Op::addiu && setup.op!=r5900::Op::daddiu && setup.op!=r5900::Op::ori)) continue;
            if (r5900::decode(pc+4,le32(bytes,at+4)).op!=r5900::Op::syscall ||
                le32(bytes,at+8)!=0x03e00008U || le32(bytes,at+12)!=0) continue;
            result.insert(pc);
        }
    }
    return result;
}

Program discover(ByteView bytes, const elf::Executable& elf, std::size_t limit,
                 std::span<const std::uint32_t> additional_entries, bool stop_at_syscall) {
    const auto word_at = [&](std::uint32_t pc) -> std::optional<std::uint32_t> {
        if (pc % 4 || !elf::executable_address(elf,pc,4,true)) return {};
        for (const auto& s : elf.segments)
            if (((s.flags & 1) || elf.inferred_permissions) && pc >= s.address && std::uint64_t(pc) + 4 <= std::uint64_t(s.address) + s.file_size)
                return le32(bytes, std::size_t(s.offset) + pc - s.address);
        return {};
    };
    const auto walk=[&](const std::set<std::uint32_t>& taken_branches) {
        Program result;
        result.entries.insert(elf.entry);
        result.entries.insert(elf.lifecycle_entries.begin(),elf.lifecycle_entries.end());
        result.entries.insert(additional_entries.begin(),additional_entries.end());
        // A delay slot is visited as an instruction but must not independently cause
        // fallthrough from an unconditional jump. A separate ordinary visit can.
        std::deque<std::pair<std::uint32_t, bool>> pending{{elf.entry, false}};
        for (const auto pc:elf.lifecycle_entries) pending.emplace_back(pc,false);
        for (auto pc : additional_entries) {
            if (!word_at(pc)) throw std::runtime_error("Additional entry is not executable ELF memory");
            pending.emplace_back(pc,false);
        }
        std::set<std::pair<std::uint32_t, bool>> visited;
        while (!pending.empty()) {
            const auto [pc, delay] = pending.front(); pending.pop_front();
            if (!visited.emplace(pc, delay).second) continue;
            const auto word = word_at(pc);
            if (!word) { result.unresolved.insert(pc); continue; }
            const auto instruction = r5900::decode(pc, *word);
            result.instructions.emplace(pc, instruction);
            if (!delay) result.ordinary.insert(pc);
            if (result.instructions.size() > limit)
                throw std::runtime_error("Analysis instruction limit exceeded ("+std::to_string(limit)+") at "+hex32(pc));
            if (delay || instruction.terminal()) continue;
            if (stop_at_syscall && instruction.op==r5900::Op::syscall) {
                // A leaf syscall wrapper can return through a dynamically registered
                // compiled handler even when its service number is not known here.
                // Discover its return epilogue without assuming syscall success.
                if (pc<=0xfffffff7U) {
                    const auto tail=word_at(pc+4),slot=word_at(pc+8);
                    if (tail && slot && *slot==0) {
                        const auto next=r5900::decode(pc+4,*tail);
                        if (next.op==r5900::Op::jr && next.rs==31) pending.emplace_back(pc+4,false);
                    }
                }
                continue;
            }
            if (instruction.branch()) {
                pending.emplace_back(pc + 4, true);
                if (!instruction.indirect()) pending.emplace_back(instruction.target(), false);
                if (instruction.op != r5900::Op::j && instruction.op != r5900::Op::jr &&
                    !pointer_flow::always_taken(instruction) && !taken_branches.contains(pc))
                    pending.emplace_back(pc + 8, false);
            } else {
                // A syscall is a runtime boundary, but analysis may inspect its continuation.
                pending.emplace_back(pc + 4, false);
            }
        }
        return result;
    };
    std::set<std::uint32_t> taken_branches;
    auto result=walk(taken_branches);
    // Prove a constant guard against the conservative graph before removing an
    // edge. Memory contents never establish a constant register here. Calls,
    // unknown writes and disagreeing incoming paths invalidate the proof.
    for (unsigned pass=0;pass<4;++pass) {
        const pointer_flow::Graph graph(result);
        const auto previous=taken_branches.size();
        for (const auto& [pc,i]:result.instructions) {
            if ((i.op!=r5900::Op::beq && i.op!=r5900::Op::beql) ||
                taken_branches.contains(pc) || pointer_flow::always_taken(i)) continue;
            const auto left=pointer_flow::constant_gpr(result,graph,pc,i.rs);
            const auto right=pointer_flow::constant_gpr(result,graph,pc,i.rt);
            if (left && right && *left==*right) taken_branches.insert(pc);
        }
        if (taken_branches.size()==previous) break;
        result=walk(taken_branches);
    }
    return result;
}
}
