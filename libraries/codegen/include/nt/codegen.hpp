#pragma once
#include <nt/analysis.hpp>

namespace nt::codegen {
// Emit native operations at compile time. No instruction decoder is linked into the output.
void emit(std::ostream& out, const analysis::Program& program);
void project(const std::filesystem::path& elf_path, const std::filesystem::path& output,
             std::span<const std::uint32_t> additional_entries = {},
             const std::filesystem::path& capture = {});
}
