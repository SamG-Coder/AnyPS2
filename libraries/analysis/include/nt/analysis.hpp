#pragma once
#include <nt/elf.hpp>
#include <nt/r5900.hpp>
#include <map>
#include <set>

namespace nt::analysis {
struct Program {
    std::map<std::uint32_t, r5900::Instruction> instructions;
    std::set<std::uint32_t> unresolved;
};
// Follow direct flow from entry. Indirect targets require explicit future evidence.
Program discover(ByteView bytes, const elf::Executable& executable, std::size_t limit = 100000,
                 std::span<const std::uint32_t> additional_entries = {}, bool stop_at_syscall = false);
void describe(std::ostream& out, const Program& program);
}
