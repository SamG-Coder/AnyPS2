#pragma once
#include <nt/elf.hpp>
#include <nt/r5900.hpp>
#include <map>
#include <set>
#include <array>
#include <optional>
#include <string_view>
#include <vector>

namespace nt::analysis {
struct Program {
    std::map<std::uint32_t, r5900::Instruction> instructions;
    std::set<std::uint32_t> unresolved;
    std::set<std::uint32_t> ordinary;
    std::set<std::uint32_t> entries;
    // Conversion-time evidence only. Never used to fold runtime table values.
    std::map<std::uint32_t,std::set<std::uint32_t>> indirect_candidates;
};
enum class CallKind { direct, conditional, indirect, direct_jump, indirect_jump, return_link, return_exception, service };
struct CallSite {
    std::uint32_t pc;
    CallKind kind;
    std::optional<std::uint32_t> target;
    bool initial_data_candidate=false;
};
// An inventory of reached transfer sites, not proof that all dynamic values or
// executable content have been found. Return addresses remain runtime values.
std::vector<CallSite> call_sites(ByteView bytes,const elf::Executable&,const Program&);
std::string_view name(CallKind kind);
// Follow direct flow from entry. Indirect targets require explicit future evidence.
Program discover(ByteView bytes, const elf::Executable& executable, std::size_t limit = 100000,
                 std::span<const std::uint32_t> additional_entries = {}, bool stop_at_syscall = false);
// Decode every word in declared EE code sections, independently of callers or
// reachability. Unknown instructions remain in the result for batch rejection.
// Ambiguous sectionless/inferred images require an explicit code classification.
Program whole_code(ByteView bytes,const elf::Executable& executable,std::size_t limit=100000);
// Scan file-backed executable segments for leaf syscall-wrapper candidates.
// Candidates are not proof that a service is implemented or used at runtime.
std::set<std::uint32_t> syscall_wrappers(ByteView bytes,const elf::Executable& executable);
struct LayoutCandidate { std::uint32_t source,call; std::array<std::uint32_t,4> fields; };
// Initial-data candidates used only with exact runtime declaration guards.
std::vector<LayoutCandidate> layout_candidates(ByteView bytes,const elf::Executable&,const Program&);
// Candidate callback targets at direct calls to simple syscall wrappers. These
// are compilation roots, never constant-folding facts about runtime state.
std::set<std::uint32_t> syscall_callbacks(ByteView bytes,const elf::Executable& executable,
                                        const Program& program,std::uint32_t syscall,
                                        std::set<std::uint32_t>* registered_calls=nullptr);
std::set<std::uint32_t> materialized_code_pointers(ByteView bytes,const elf::Executable& executable,
                                                const Program& program);
struct CopiedCode { std::uint32_t source,destination,size; std::set<std::uint32_t> entry_offsets; };
// Code a program copies out of its own file-backed image and registers as a
// system-call handler. Evidence requires a constant SetSyscall handler outside
// every loaded segment and a reached call whose constant (destination, source,
// size) arguments, in that ABI order, cover it. Other copies through the same
// routine are treated as code from their first word. Entries are handlers and
// words inside the copy that address decodable code within it. They are
// compilation roots only; the program still performs the copy at runtime.
std::vector<CopiedCode> copied_code(ByteView bytes,const elf::Executable& executable,const Program& program);
std::set<std::uint32_t> thread_entries(ByteView bytes,const elf::Executable& executable,const Program& program);
struct PointerReference {
    std::uint32_t site,target;
    bool argument=false;
    bool callable=false;
    bool data_read=false;
    std::uint32_t callee=0;
    unsigned argument_index=0;
};
struct CallbackFlowGap {
    std::uint32_t site,target,field;
    std::vector<std::uint32_t> consumers;
};
// Diagnostic candidates only: a matching function/context layout does not
// prove that a store and consumer use the same object. Never adds code roots.
std::vector<CallbackFlowGap> callback_flow_gaps(ByteView bytes,const elf::Executable&,const Program&,
                                              std::span<const PointerReference> references);
// Stores become roots only when a matching indirect-call consumer is found.
// Other address references remain available as unresolved classification evidence.
std::set<std::uint32_t> stored_code_pointers(ByteView bytes,const elf::Executable& executable,const Program& program,
    std::vector<PointerReference>* references=nullptr);
// Resolve arguments used as calls, or stored into slots with observed call uses.
std::set<std::uint32_t> registered_code_pointers(ByteView bytes,const elf::Executable& executable,const Program& program,
    std::vector<PointerReference>* references=nullptr);
// Candidate functions loaded from constant-offset tables installed in objects.
// Runtime table reads and indirect dispatch remain dynamic.
std::set<std::uint32_t> object_table_entries(ByteView bytes,const elf::Executable& executable,const Program& program);
// Bounded indexed switch-table candidates; no runtime guard is removed.
std::set<std::uint32_t> bounded_switch_entries(ByteView bytes,const elf::Executable& executable,const Program& program,
    std::map<std::uint32_t,std::set<std::uint32_t>>* candidate_edges=nullptr);
// Initial-data candidates for indirect transfers through any register.
// Native execution keeps its original loads and indirect branch.
std::set<std::uint32_t> initial_indirect_entries(ByteView bytes,const elf::Executable&,const Program&);
// Recover bounded legacy lifecycle lists whose headers are explicitly loaded
// from initial file data. Candidates only; do not fold runtime callback reads.
std::set<std::uint32_t> referenced_lifecycle_entries(ByteView bytes,const elf::Executable&,const Program&);
// Initializer arrays passed as constant [begin, end) arguments to a function
// that makes indirect calls. Every slot must address decodable code; otherwise
// the range is data. Candidates are compilation roots, not runtime values.
std::set<std::uint32_t> bounded_pointer_range_entries(ByteView bytes,const elf::Executable&,const Program&);
void describe(std::ostream& out, const Program& program);
}
