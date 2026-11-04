#pragma once
#include <nt/loop_pipeline.hpp>
#include <ostream>
#include <string_view>

namespace nt::render {
// Only the numerical range/independence gate. Region entry, live-out state and
// startup/drain equivalence remain separate requirements for installation.
struct ElementRangeGuard {
    struct Range { int base=-1; std::int32_t offset=0,stride=0; bool selected=false; };
    unsigned rows=0,counter=0,step=0,minimum_element=0,output_base=0;
    std::int32_t output_first=0,output_last=0,output_stride=0;
    std::array<std::int32_t,16> steps{};
    std::vector<Range> reads;
    std::vector<AffineInteger> integer_writes;
};
ElementRangeGuard analyze_element_range(std::span<const vu::Pair>,unsigned unit);
// uint32_t symbol(const uint32_t* element_parameters). Returns VectorRangeStatus.
// The generated function checks launch values directly; it decodes no PS2 code.
void emit_element_range_llvm(std::ostream&,const ElementRangeGuard&,std::string_view symbol);
}
