#pragma once
#include <nt/loop_pipeline.hpp>
#include <ostream>
#include <string_view>

namespace nt::render {
// Only the numerical range/independence gate. Region entry, live-out state and
// startup/drain equivalence remain separate requirements for installation.
struct ElementRangeGuard {
    // ordered: bit i lets this read overlap output stream i, whose stores all
    // share its stride, when the Order checks prove at launch that every row it
    // reads is read before any iteration writes it; the kernel's entry-memory
    // input then equals what the original loop read. Disjoint ranges also pass.
    struct Range { int base=-1; std::int32_t offset=0,stride=0; bool selected=false; unsigned ordered=0; };
    // One raw load and one raw store per iteration with equal positive stride.
    // With d = load row - store row at entry, they touch a common row only if d
    // is a multiple of the stride; the store is then d/stride iterations later.
    // Safe: no common row, d >= stride, or d == 0 with the load first in the body.
    struct Order { int load_base=-1,store_base=-1; std::int32_t load_offset=0,store_offset=0,stride=0; bool load_first=false;
        unsigned read=0,output=0; };  // the read (guard.reads index) and output stream it relates
    std::vector<Order> orders;
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
