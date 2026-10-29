#pragma once
#include <nt/r5900.hpp>
#include <iosfwd>
#include <span>
#include <filesystem>
#include <vector>

namespace nt::ee_codegen {
inline constexpr std::size_t function_instruction_limit=512;
inline constexpr std::size_t program_instruction_limit=262144;
struct PayloadMapping { std::uint32_t source,destination,size; std::vector<std::uint32_t> entry_offsets; };
struct CodePayload { std::uint32_t address; std::vector<std::uint8_t> bytes; };
struct CodeRange { std::uint32_t address,size; };
// Emits a sorted, bounded set of compiled instructions. Additional ranges protect
// executable ELF contents that have not yet been reached by discovery.
void emit_llvm(std::ostream& output,std::span<const r5900::Instruction> instructions,
               std::span<const CodeRange> protected_code = {},
               std::span<const CodePayload> payloads = {});
// Discover entry flow and recognized native syscall continuations, then emit a project.
void project(const std::filesystem::path& input,const std::filesystem::path& output,
             std::span<const PayloadMapping> payloads = {});
}
