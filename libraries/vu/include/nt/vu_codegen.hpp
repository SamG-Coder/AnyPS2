#pragma once
#include <nt/vu_program.hpp>
#include <iosfwd>
#include <string_view>

namespace nt::vu {
// Compile one basic block ending in a branch or E flag and its delay pair. Unsupported semantics fail before
// output. The generated function returns Result and atomically commits State only
// on success; unsupported numeric inputs leave the input region state unchanged.
void emit_llvm(std::ostream& output,std::span<const Pair> pairs,unsigned unit,std::string_view symbol="nt_vu_generated");
// Compile reachable blocks from explicit entries into one module with native
// block-table and count accessors. Indirect destinations require explicit entries.
void emit_program_llvm(std::ostream& output,std::span<const Pair> pairs,unsigned unit,
    std::span<const unsigned> entries,std::string_view symbol="nt_vu_program");
}
