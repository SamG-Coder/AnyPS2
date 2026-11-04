#pragma once
#include <nt/element_range.hpp>

namespace nt::render {
struct VectorRegionPlan {
    unsigned unit=0,head=0,exit=0,startup=0,drain=0;
    std::vector<vu::Pair> body;
    PipelinedLoop loop;
    ElementRangeGuard range;
};
// program is the complete declared image and entries includes all native exports.
// Rejects indirect entry uncertainty, interior entries and unproved side effects.
VectorRegionPlan analyze_vector_region(std::span<const vu::Pair> program,unsigned unit,
    std::span<const unsigned> entries,unsigned head);
// Separate reproducible modules: original boundary code, recovered elements,
// range guard plus descriptor. Link with the native region service.
void emit_vector_region_llvm(std::ostream& boundary,std::ostream& elements,std::ostream& binding,
    const VectorRegionPlan&,std::string_view symbol);
}
