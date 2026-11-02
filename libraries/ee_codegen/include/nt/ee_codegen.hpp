#pragma once
#include <nt/binary.hpp>
#include <nt/r5900.hpp>
#include <iosfwd>
#include <map>
#include <span>
#include <filesystem>
#include <string>
#include <vector>
#include <array>
#include <functional>

namespace nt::ee_codegen {
inline constexpr std::size_t function_instruction_limit=512;
// Allow an eight-MiB executable while keeping discovery and emission bounded.
// Individual native functions and compilation units retain their smaller caps.
inline constexpr std::size_t program_instruction_limit=2*1024*1024;
struct PayloadMapping { std::uint32_t source,destination,size; std::vector<std::uint32_t> entry_offsets; };
struct CodePayload { std::uint32_t address; std::vector<std::uint8_t> bytes; };
using LayoutDeclaration=std::array<std::uint32_t,4>;
struct CodeRange { std::uint32_t address,size,image=0; }; // image 0 is permanently resident.
struct ExecutableImage { std::uint32_t entry,id; std::vector<CodePayload> segments; };
// Emits a sorted, bounded set of compiled instructions. Additional ranges protect
// executable ELF contents that have not yet been reached by discovery.
// An optional stream factory splits code into numbered, bounded LLVM modules;
// output then contains shared metadata and dispatch. Streams must remain valid
// until emission returns. Without the factory, output contains the whole module.
void emit_llvm(std::ostream& output,std::span<const r5900::Instruction> instructions,
               std::span<const CodeRange> protected_code = {},
               std::span<const CodePayload> payloads = {},
               std::span<const LayoutDeclaration> layouts = {},
               std::span<const ExecutableImage> images = {},
               const std::function<std::ostream&(std::size_t)>& module_output = {},
               std::uint64_t input_identity=0);
// Discover entry flow and recognized native syscall continuations, then emit a
// project. Optional native-stop directories supply checked offline call edges;
// they never become runtime inputs or establish complete indirect coverage.
void project(const std::filesystem::path& input,const std::filesystem::path& output,
             std::span<const PayloadMapping> payloads = {},
             std::span<const std::filesystem::path> images = {},
             std::span<const std::filesystem::path> observations = {});
struct FlowVerification {
    std::size_t instructions=0,passes=0,unmapped_targets=0,unresolved_indirect=0;
    std::size_t unclassified_references=0,record_binding_candidates=0,service_boundaries=0;
    bool lowering_supported=false,layout_supported=false;
    bool closed() const {
        return instructions && lowering_supported && layout_supported && !unmapped_targets &&
            !unresolved_indirect && !unclassified_references;
    }
};
// Run the same discovery closure as conversion and publish its complete gap
// inventory, without emitting LLVM, game data or a native build. A closed
// reached-flow inventory is not proof of dynamic content or game compatibility.
FlowVerification verify_flow(const std::filesystem::path& input,const std::filesystem::path& output,
                             std::span<const PayloadMapping> payloads = {},
                             std::span<const std::filesystem::path> images = {},
                             std::span<const std::filesystem::path> observations = {});
// Unsupported operations and encodings in an already decoded instruction list.
// Empty means this list can be lowered. No text is emitted.
struct EeRejection {
    struct Family { std::string name; std::uint32_t first=0; std::size_t count=0; };
    std::vector<Family> families;
    std::vector<std::string> constraints;
    std::size_t omitted_constraints=0;
    bool empty() const { return families.empty() && constraints.empty(); }
    std::string text() const;
};
EeRejection reject_unsupported(std::span<const r5900::Instruction> instructions);
// Coverage of one executable image. convertible is true only when discovery and
// lowering both accept the reached instructions. Indirect sites remain visible
// because their runtime targets are not a proof of recovered code.
struct EeAssessment {
    std::size_t instructions=0;
    std::size_t declared_executable_words=0;
    std::size_t unresolved_count=0;
    std::vector<std::uint32_t> unresolved_sample;
    std::size_t unclassified_pointer_count=0;
    std::vector<std::uint32_t> unclassified_pointer_sample;
    std::size_t indirect_sites=0;
    std::uint32_t first_indirect=0;
    std::map<std::uint32_t,std::size_t> known_syscalls;
    std::size_t unknown_syscall_sites=0;
    std::size_t wrapper_candidates=0;
    std::vector<std::uint32_t> registered_calls;
    std::size_t address_space_requests=0;
    std::uint32_t first_address_space_request=0;
    EeRejection rejection;
    std::string layout_error;
    bool convertible=false;
};
EeAssessment assess(ByteView bytes,std::span<const PayloadMapping> payloads = {});
}
