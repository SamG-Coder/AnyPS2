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

namespace nt::ee_codegen {
inline constexpr std::size_t function_instruction_limit=512;
inline constexpr std::size_t program_instruction_limit=262144;
struct PayloadMapping { std::uint32_t source,destination,size; std::vector<std::uint32_t> entry_offsets; };
struct CodePayload { std::uint32_t address; std::vector<std::uint8_t> bytes; };
using LayoutDeclaration=std::array<std::uint32_t,4>;
struct CodeRange { std::uint32_t address,size; };
// Emits a sorted, bounded set of compiled instructions. Additional ranges protect
// executable ELF contents that have not yet been reached by discovery.
void emit_llvm(std::ostream& output,std::span<const r5900::Instruction> instructions,
               std::span<const CodeRange> protected_code = {},
               std::span<const CodePayload> payloads = {},
               std::span<const LayoutDeclaration> layouts = {});
// Discover entry flow and recognized native syscall continuations, then emit a project.
void project(const std::filesystem::path& input,const std::filesystem::path& output,
             std::span<const PayloadMapping> payloads = {});
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
