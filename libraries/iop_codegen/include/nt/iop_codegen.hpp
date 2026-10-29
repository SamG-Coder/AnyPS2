#pragma once
#include <nt/iop_module.hpp>
#include <iosfwd>

namespace nt::iop_codegen {
// Conversion follows direct control flow and caller-supplied additional roots.
// Unsupported reachable instructions fail before any generated text is emitted.
void emit_llvm(std::ostream& output,const elf::IopModule& module,
               std::span<const std::uint32_t> additional_entries={});
// Modules must already be relocated into disjoint data-arena ranges.
void emit_bundle(std::ostream& output,std::span<const elf::IopModule> modules,
                 std::span<const std::uint32_t> additional_entries={});
struct ModuleSource { std::filesystem::path path; std::uint32_t base; };
void project_bundle(std::span<const ModuleSource> inputs,const std::filesystem::path& output);
void project(const std::filesystem::path& input,const std::filesystem::path& output,std::uint32_t base);
}
