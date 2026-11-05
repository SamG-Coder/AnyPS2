#pragma once
#include <nt/vu_program.hpp>
#include <iosfwd>
#include <string_view>

namespace nt::vu {
// Compile one basic block ending in a branch or E flag and its delay pair. Unsupported semantics fail before
// output. The generated function returns Result and atomically commits State only
// on success; unsupported numeric inputs leave the input region state unchanged.
void emit_llvm(std::ostream& output,std::span<const Pair> pairs,unsigned unit,std::string_view symbol="nt_vu_generated");
// Diagnostic code generation: every VU data load also calls
// nt_vu_observe_load(row), which the linking program must define. Off by
// default; it changes no VU semantics and is never needed for execution.
void set_observe_loads(bool enabled);
// Compile reachable blocks from explicit entries into one module with native
// block-table and count accessors. Indirect destinations require explicit entries.
// A bounded number of subsequent E-stop continuations may be discovered offline.
void emit_program_llvm(std::ostream& output,std::span<const Pair> pairs,unsigned unit,
    std::span<const unsigned> entries,std::string_view symbol="nt_vu_program",unsigned continuation_depth=0,
    bool all_entries=false);
// The same blocks as one Vulkan compute kernel (vu_gpu.hpp job interface),
// for linking with the shared numeric helpers compiled to SPIR-V.
void emit_gpu_program_llvm(std::ostream& output,std::span<const Pair> pairs,unsigned unit,
    std::span<const unsigned> entries,std::string_view symbol="nt_vu_program",bool all_entries=false);
void emit_catalog_llvm(std::ostream& output,std::span<const std::vector<Pair>> images,
    unsigned unit,std::string_view symbol);
// all_entries compiles every pair in the declared image, including independently
// entered delay pairs. Fallthrough beyond the image remains an unmapped boundary;
// it is never treated as successful completion or decoded at runtime.
}
