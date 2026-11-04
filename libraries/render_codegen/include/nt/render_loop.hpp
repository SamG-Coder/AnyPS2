#pragma once
#include <nt/render_codegen.hpp>
#include <array>

namespace nt::render {
// Conversion-time provenance. Rows are source quadwords, not GPU allocations.
struct VertexStream {
    unsigned vector=0,base_register=0;
    std::int32_t offset_rows=0;
    unsigned stride_rows=0;
};
class VertexLoop {
public:
    const Kernel& kernel() const { return kernel_; }
    const std::vector<VertexStream>& reads() const { return reads_; }
    const VertexStream& write() const { return write_; }
    const std::array<int,16>& steps() const { return steps_; }
    unsigned counter() const { return counter_; }
    unsigned unit() const { return unit_; }
    unsigned source_pairs() const { return pairs_; }
private:
    VertexLoop() = default;
    Kernel kernel_;
    std::vector<VertexStream> reads_;
    VertexStream write_;
    std::array<int,16> steps_{};
    unsigned counter_=0,unit_=0,pairs_=0;
    friend VertexLoop analyze_vertex_loop(std::span<const vu::Pair>,unsigned);
};
// Recognizes load -> arithmetic -> store -> induction -> BNE -> nop loops.
// This proves an isolated memory computation under bind-time guards, not that
// externally visible registers/flags or an entire VU launch can be discarded.
VertexLoop analyze_vertex_loop(std::span<const vu::Pair>,unsigned unit);
struct LoopBindings {
    // [count, one word per live input]. Vertex words are source-word offsets;
    // uniform words contain the shared float32 bits. There is no item packing.
    std::vector<std::uint32_t> parameters;
    unsigned output_words=0,source_output_row=0,source_output_stride=0;
};
// Rejects zero-count do/while, wrap, repeated output rows and every source
// read/output overlap, including dead reads. Source must remain immutable until
// execution completes. The returned parameters are an owned uniform snapshot.
LoopBindings bind_vertex_loop(const VertexLoop&,std::span<const std::uint16_t,16> integers,
    std::span<const std::uint32_t> source);
// source is an attribute buffer, parameters are small per-draw metadata, output
// is a disjoint packed float4 buffer. No source-register bank is in this ABI.
// CPU runs the complete batch; GPU runs one independent item per invocation.
void emit_cpu_llvm(std::ostream&,const VertexLoop&,NumericPolicy);
void emit_spirv_llvm(std::ostream&,const VertexLoop&,NumericPolicy);
void write_manifest(std::ostream&,const VertexLoop&);
}
