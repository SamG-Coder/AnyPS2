#pragma once
#include <nt/r5900.hpp>
#include <iosfwd>
#include <span>
#include <filesystem>

namespace nt::ee_codegen {
inline constexpr std::size_t function_instruction_limit=512;
inline constexpr std::size_t program_instruction_limit=262144;
struct CodeRange { std::uint32_t address,size; };
// Emits a sorted, bounded set of compiled instructions. Additional ranges protect
// executable ELF contents that have not yet been reached by discovery.
void emit_llvm(std::ostream& output,std::span<const r5900::Instruction> instructions,
               std::span<const CodeRange> protected_code = {});
// Discover entry flow and recognized native syscall continuations, then emit a project.
void project(const std::filesystem::path& input,const std::filesystem::path& output);
}
