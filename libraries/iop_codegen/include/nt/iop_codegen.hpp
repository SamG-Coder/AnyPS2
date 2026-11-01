#pragma once
#include <nt/iop_module.hpp>
#include <nt/iop_ir.hpp>
#include <iosfwd>
#include <functional>

namespace nt::iop_codegen {
// Conversion follows direct control flow and caller-supplied additional roots.
// Unsupported reachable instructions fail before any generated text is emitted.
void emit_llvm(std::ostream& output,const elf::IopModule& module,
               std::span<const std::uint32_t> additional_entries={});
// Emit an already diagnosed program. The caller must not pass a partial program.
void emit_program(std::ostream& output,const ir::Program& program);
// The primary stream owns dispatch and address checks. Code streams contain
// fixed 16-page address groups, so adding code does not renumber later files.
// An absent stream factory retains the single-module format.
using CodeStream=std::function<std::ostream&(std::size_t)>;
void emit_program(std::ostream& output,const ir::Program& program,const CodeStream& code_stream);
// Modules must already be relocated into disjoint data-arena ranges.
void emit_bundle(std::ostream& output,std::span<const elf::IopModule> modules,
                 std::span<const std::uint32_t> additional_entries={});
// sealed compiles a later LoadStartModule image. It is not native_romdrv.
// Both flags set is rejected: ROMDRV is the only native archive replacement.
struct ModuleSource { std::filesystem::path path; std::uint32_t base; bool native_romdrv=false; bool sealed=false; };
void project_bundle(std::span<const ModuleSource> inputs,const std::filesystem::path& output);
void project(const std::filesystem::path& input,const std::filesystem::path& output,std::uint32_t base);
}
