#include <nt/analysis.hpp>
#include <deque>
#include <optional>
#include <stdexcept>

namespace nt::analysis {
Program discover(ByteView bytes, const elf::Executable& elf, std::size_t limit,
                 std::span<const std::uint32_t> additional_entries, bool stop_at_syscall) {
    const auto word_at = [&](std::uint32_t pc) -> std::optional<std::uint32_t> {
        if (pc % 4) return {};
        for (const auto& s : elf.segments)
            if ((s.flags & 1) && pc >= s.address && std::uint64_t(pc) + 4 <= std::uint64_t(s.address) + s.file_size)
                return le32(bytes, std::size_t(s.offset) + pc - s.address);
        return {};
    };
    Program result;
    // A delay slot is visited as an instruction but must not independently cause
    // fallthrough from an unconditional jump. A separate ordinary visit can.
    std::deque<std::pair<std::uint32_t, bool>> pending{{elf.entry, false}};
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
        if (result.instructions.size() > limit) throw std::runtime_error("Analysis instruction limit exceeded");
        if (delay || instruction.terminal() || (stop_at_syscall && instruction.op==r5900::Op::syscall)) continue;
        if (instruction.branch()) {
            pending.emplace_back(pc + 4, true);
            if (!instruction.indirect()) pending.emplace_back(instruction.target(), false);
            if (instruction.op != r5900::Op::j && instruction.op != r5900::Op::jr)
                pending.emplace_back(pc + 8, false);
        } else {
            // A syscall is a runtime boundary, but analysis may inspect its continuation.
            pending.emplace_back(pc + 4, false);
        }
    }
    return result;
}
namespace {
std::optional<std::uint32_t> initial_word(ByteView bytes,const elf::Executable& elf,std::uint32_t address) {
    if (address>=0x80000000U && address<0xc0000000U) address&=0x1fffffffU;
    if (address%4) return {};
    for (const auto& segment:elf.segments)
        if (address>=segment.address && std::uint64_t(address)+4<=std::uint64_t(segment.address)+segment.file_size)
            return le32(bytes,std::size_t(segment.offset)+address-segment.address);
    return {};
}
// Bounded backward slicing is used only to propose code for compilation. A load
// reads the initial ELF image, not a claim that writable data stays constant.
std::optional<std::uint32_t> candidate(ByteView bytes,const elf::Executable& elf,const Program& program,
                                     std::uint32_t before,unsigned reg,unsigned& fuel,std::uint32_t call_site) {
    using r5900::Op;
    if (!reg) return 0;
    while (before>=4 && fuel) {
        --fuel; before-=4;
        const auto found=program.instructions.find(before);
        if (found==program.instructions.end()) return {};
        const auto& i=found->second;
        if (i.op==Op::jal) {
            if (before==call_site && reg!=31) continue;
            // Saved registers are ABI candidates across calls, never proven values.
            if (reg>=16 && reg<=23) continue;
            return {};
        }
        if (i.branch() || i.terminal() || i.op==Op::syscall) return {};
        unsigned destination=0;
        switch (i.op) {
        case Op::sw: case Op::sd: case Op::sq: case Op::sb: case Op::sh: continue;
        case Op::lui: case Op::addiu: case Op::daddiu: case Op::ori: case Op::lw: case Op::lwu:
            destination=i.rt; break;
        case Op::addu: case Op::daddu: case Op::bit_or: case Op::sll:
            destination=i.rd; break;
        default: return {};
        }
        if (destination!=reg) continue;
        if (i.op==Op::lui) return static_cast<std::uint32_t>(i.immediate)<<16;
        const auto a=candidate(bytes,elf,program,before,i.rs,fuel,call_site);
        if (!a) return {};
        if (i.op==Op::addiu || i.op==Op::daddiu) return *a+static_cast<std::uint32_t>(i.immediate);
        if (i.op==Op::ori) return *a|static_cast<std::uint16_t>(i.immediate);
        if (i.op==Op::lw || i.op==Op::lwu) return initial_word(bytes,elf,*a+static_cast<std::uint32_t>(i.immediate));
        const auto b=candidate(bytes,elf,program,before,i.rt,fuel,call_site);
        if (!b) return {};
        if (i.op==Op::sll) return *b<<i.sa;
        return i.op==Op::bit_or ? *a|*b : *a+*b;
    }
    return {};
}
}
std::set<std::uint32_t> syscall_callbacks(ByteView bytes,const elf::Executable& elf,
                                        const Program& program,std::uint32_t syscall,std::set<std::uint32_t>* registered_calls) {
    using r5900::Op;
    std::set<std::uint32_t> result;
    for (const auto& [pc,call]:program.instructions) {
        if (call.op!=Op::jal || pc>0xfffffff7U) continue;
        const auto setup=program.instructions.find(call.target());
        const auto boundary=program.instructions.find(call.target()+4);
        if (setup==program.instructions.end() || boundary==program.instructions.end()) continue;
        const auto& i=setup->second;
        if (i.rs || i.rt!=3 || (i.op!=Op::addiu && i.op!=Op::daddiu && i.op!=Op::ori) ||
            static_cast<std::uint32_t>(i.immediate)!=syscall || boundary->second.op!=Op::syscall) continue;
        // Include the call's delay slot when recovering argument a1.
        unsigned fuel=128;
        const auto target=candidate(bytes,elf,program,pc+8,5,fuel,pc);
        if (!target || !*target) continue;
        auto address=*target;
        if (address>=0x80000000U && address<0xc0000000U) address&=0x1fffffffU;
        if (address%4) continue;
        for (const auto& segment:elf.segments)
            if ((segment.flags&1) && address>=segment.address &&
                std::uint64_t(address)+4<=std::uint64_t(segment.address)+segment.file_size)
            {
                result.insert(address);
                if (registered_calls) {
                    unsigned number_fuel=128;
                    const auto number=candidate(bytes,elf,program,pc+8,4,number_fuel,pc);
                    if (number && *number<256) registered_calls->insert(*number);
                }
            }
    }
    return result;
}
std::set<std::uint32_t> materialized_code_pointers(ByteView bytes,const elf::Executable& elf,const Program& program) {
    using r5900::Op;
    std::set<std::uint32_t> result;
    for (const auto& [pc,i]:program.instructions) {
        unsigned reg=0;
        switch (i.op) {
        case Op::lui: case Op::ori: case Op::addiu: case Op::daddiu: case Op::lw: case Op::lwu: reg=i.rt; break;
        case Op::addu: case Op::daddu: case Op::bit_or: reg=i.rd; break;
        default: continue;
        }
        // Returned pointer candidates only; argument/data-address materialization
        // alone is not evidence of an executable entry.
        if (reg!=2 || pc>0xfffffffbU) continue;
        unsigned fuel=128;
        const auto value=candidate(bytes,elf,program,pc+4,reg,fuel,0xffffffffU);
        if (!value) continue;
        const auto address=*value>=0x80000000U && *value<0xc0000000U ? *value&0x1fffffffU : *value;
        if (address%4) continue;
        for (const auto& segment:elf.segments)
            if ((segment.flags&1) && address>=segment.address && std::uint64_t(address)+4<=std::uint64_t(segment.address)+segment.file_size)
                result.insert(address);
    }
    return result;
}
// Recover entry pointers from static descriptors and local stack descriptors.
// These are additional compilation candidates, not proofs of runtime values.
std::set<std::uint32_t> thread_entries(ByteView bytes,const elf::Executable& elf,const Program& program) {
    using r5900::Op;
    std::set<std::uint32_t> result;
    for (const auto& [pc,call]:program.instructions) {
        if (call.op!=Op::jal || pc>0xfffffff7U) continue;
        const auto setup=program.instructions.find(call.target());
        const auto boundary=program.instructions.find(call.target()+4);
        if (setup==program.instructions.end() || boundary==program.instructions.end()) continue;
        const auto& instruction=setup->second;
        if (instruction.rs || instruction.rt!=3 || instruction.immediate!=0x20 ||
            (instruction.op!=Op::addiu && instruction.op!=Op::daddiu && instruction.op!=Op::ori) ||
            boundary->second.op!=Op::syscall) continue;
        unsigned fuel=128;
        const auto descriptor=candidate(bytes,elf,program,pc+8,4,fuel,pc);
        std::optional<std::uint32_t> entry;

        // The common local form uses a0 = sp + offset. Walk only the local
        // straight-line sequence; changes to sp or another call end the search.
        std::optional<std::int32_t> offset;
        auto before=pc+8;
        for (unsigned steps=0;steps<128 && before>=4 && !offset;++steps) {
            before-=4;
            const auto found=program.instructions.find(before);
            if (found==program.instructions.end()) break;
            const auto& i=found->second;
            if (before==pc) continue;
            if (i.branch() || i.terminal() || i.op==Op::syscall) break;
            if ((i.op==Op::addiu || i.op==Op::daddiu) && i.rt==4 && i.rs==29)
                offset=static_cast<std::int32_t>(i.immediate)+4;
            if ((i.op==Op::addu || i.op==Op::daddu || i.op==Op::bit_or) && i.rd==4 &&
                ((i.rs==29 && !i.rt) || (i.rt==29 && !i.rs))) offset=4;
        }
        before=pc+8;
        for (unsigned steps=0;steps<128 && before>=4;++steps) {
            before-=4;
            const auto found=program.instructions.find(before);
            if (found==program.instructions.end()) break;
            const auto& i=found->second;
            if (before==pc) continue;
            if (i.branch() || i.terminal() || i.op==Op::syscall) break;
            if (i.op==Op::sw) {
                bool matches=offset && i.rs==29 && i.immediate==*offset;
                if (descriptor) {
                    unsigned base_fuel=128;
                    const auto base=candidate(bytes,elf,program,before,i.rs,base_fuel,pc);
                    matches|=base && *base+static_cast<std::uint32_t>(i.immediate)==*descriptor+4;
                }
                if (matches) {
                    unsigned value_fuel=128;
                    entry=candidate(bytes,elf,program,before,i.rt,value_fuel,pc);
                    // An unresolved last store must not fall back to initial data.
                    if (!entry) entry=0;
                    break;
                }
            }
            switch (i.op) {
            case Op::sw: case Op::sd: case Op::sq: case Op::sb: case Op::sh: break;
            case Op::lui: case Op::ori: case Op::addiu: case Op::daddiu: case Op::lw: case Op::lwu:
                if (i.rt==29) before=0;
                break;
            case Op::addu: case Op::daddu: case Op::bit_or: case Op::sll:
                if (i.rd==29) before=0;
                break;
            default: before=0; break;
            }
        }
        if (!entry && descriptor && *descriptor<=0xfffffffbU) entry=initial_word(bytes,elf,*descriptor+4);
        if (!entry) continue;
        const auto address=*entry>=0x80000000U && *entry<0xc0000000U ? *entry&0x1fffffffU : *entry;
        if (address%4) continue;
        for (const auto& segment:elf.segments)
            if ((segment.flags&1) && address>=segment.address &&
                std::uint64_t(address)+4<=std::uint64_t(segment.address)+segment.file_size)
                result.insert(address);
    }
    return result;
}
void describe(std::ostream& out, const Program& p) {
    out << "Discovered instructions: " << p.instructions.size() << '\n';
    for (const auto& [pc, i] : p.instructions) {
        out << hex32(pc) << ' ' << hex32(i.word) << ' ' << r5900::name(i.op);
        if (i.branch()) out << (i.indirect() ? " indirect" : " -> " + hex32(i.target()));
        out << '\n';
    }
    for (auto pc : p.unresolved) out << "Unmapped target: " << hex32(pc) << '\n';
}
}
