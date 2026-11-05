#pragma once
#include <nt/loop_pipeline.hpp>
#include <ostream>
#include <string_view>

namespace nt::render {
// Only the numerical range/independence gate. Region entry, live-out state and
// startup/drain equivalence remain separate requirements for installation.
struct ElementRangeGuard {
    // ahead_of: index of the output stream this read precedes. It reads through
    // the same base and stride at or after every row that stream writes, so each
    // row is read no later than the iteration that writes it, and the kernel's
    // entry-memory input equals what the original loop read. It is exempt from
    // overlap with that stream only.
    struct Range { int base=-1; std::int32_t offset=0,stride=0; bool selected=false; int ahead_of=-1; };
    // One forward output stream per VI base; rows first..last of each element.
    struct Output { unsigned base=0; std::int32_t first=0,last=0,stride=0; };
    unsigned rows=0,counter=0,step=0,minimum_element=0;
    // 1 when the counter update is in the pair before the backedge: the branch
    // then compares the pre-update value and the loop runs counter/step+1 times.
    unsigned trip_offset=0;
    std::vector<Output> outputs;
    std::array<std::int32_t,16> steps{};
    std::vector<Range> reads;
    std::vector<AffineInteger> integer_writes;
};
ElementRangeGuard analyze_element_range(std::span<const vu::Pair>,unsigned unit);
// uint32_t symbol(const uint32_t* element_parameters). Returns VectorRangeStatus.
// The generated function checks launch values directly; it decodes no PS2 code.
void emit_element_range_llvm(std::ostream&,const ElementRangeGuard&,std::string_view symbol);
}
