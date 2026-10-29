#include <nt/analysis.hpp>
#include <deque>
#include <optional>
#include <stdexcept>

namespace nt::analysis {
Program discover(ByteView bytes, const elf::Executable& elf, std::size_t limit,
                 std::span<const std::uint32_t> additional_entries) {
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
        if (delay || instruction.terminal()) continue;
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
