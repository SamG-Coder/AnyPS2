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
// Scan file-backed executable segments for leaf syscall-wrapper candidates.
// Candidates are not proof that a service is implemented or used at runtime.
std::set<std::uint32_t> syscall_wrappers(ByteView bytes,const elf::Executable& executable);
// Candidate callback targets at direct calls to simple syscall wrappers. These
// are compilation roots, never constant-folding facts about runtime state.
std::set<std::uint32_t> syscall_callbacks(ByteView bytes,const elf::Executable& executable,
                                        const Program& program,std::uint32_t syscall,
                                        std::set<std::uint32_t>* registered_calls=nullptr);
std::set<std::uint32_t> materialized_code_pointers(ByteView bytes,const elf::Executable& executable,
                                                const Program& program);
std::set<std::uint32_t> thread_entries(ByteView bytes,const elf::Executable& executable,const Program& program);
// Compilation candidates stored into runtime callback tables; not immutable values.
std::set<std::uint32_t> stored_code_pointers(ByteView bytes,const elf::Executable& executable,const Program& program);
// Resolve constant arguments that a bounded direct callee stores through copies.
std::set<std::uint32_t> registered_code_pointers(ByteView bytes,const elf::Executable& executable,const Program& program);
// Candidate functions loaded from constant-offset tables installed in objects.
// Runtime table reads and indirect dispatch remain dynamic.
std::set<std::uint32_t> object_table_entries(ByteView bytes,const elf::Executable& executable,const Program& program);
// Bounded indexed switch-table candidates; no runtime guard is removed.
std::set<std::uint32_t> bounded_switch_entries(ByteView bytes,const elf::Executable& executable,const Program& program);
void describe(std::ostream& out, const Program& program);
}
