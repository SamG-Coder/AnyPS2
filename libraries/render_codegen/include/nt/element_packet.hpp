#pragma once
#include <nt/loop_pipeline.hpp>
#include <nt/vector_draw.hpp>
#include <ostream>
#include <string_view>

namespace nt::render {
// Conversion-time relationship between one iteration's stores and a declared
// PACKED vertex tag. This is not a proof that the containing region is replaceable.
struct ElementPacketBinding {
    unsigned base=0;
    std::int32_t first_row=0;
    unsigned rows_per_vertex=0,element_words=0,minimum_element=0;
    std::vector<graphics::VectorRegister> vertex;
};
// The caller supplies the proven payload origin, in the same symbolic base/row
// coordinate system as ElementStore. No address/value is guessed from captures.
ElementPacketBinding bind_element_packet(const PipelinedLoop&,unsigned base,std::int32_t first_row,
    std::uint64_t tag,std::uint64_t registers);
// Immutable native layout symbols, appended to an LLVM module. No runtime
// instruction analysis or generated C++ is needed to bind the compiled kernel.
void emit_element_packet_llvm(std::ostream&,const ElementPacketBinding&,std::string_view symbol);
}
