#pragma once
#include <nt/gpu_ir.hpp>
#include <nt/vu_program.hpp>
#include <compare>
#include <cstdint>
#include <ostream>
#include <span>
#include <vector>

namespace nt::render {
enum class RegisterFile { vector, accumulator };
struct Lane {
    RegisterFile file=RegisterFile::vector;
    unsigned index=0,component=0;
    auto operator<=>(const Lane&) const = default;
};
enum class InputRate { vertex, uniform };
struct Input {
    Lane source;
    InputRate rate=InputRate::vertex;
};
struct Kernel {
    gpu::Program arithmetic;
    std::vector<Input> inputs;
    std::vector<Lane> outputs;
    unsigned source_pairs=0;
};
struct Layout {
    unsigned vertex_words=0,uniform_words=0,output_words=0;
    static constexpr unsigned maximum_items=65536;
};
// Extracts a mathematical region, not a complete VU launch. The caller still
// owes control/data dependency, flag liveness and independent-item proofs.
// Uniform annotations describe a binding contract; they are not inferred.
Kernel lift_vu_arithmetic(std::span<const vu::Pair> pairs,unsigned unit,
    std::span<const Lane> outputs,std::span<const unsigned> uniform_vectors={});
void validate(const Kernel&);
Layout layout(const Kernel&);
// An explicit opt-in: host/GPU float32 rounding is not PS2 VU arithmetic.
// No fast-math or reassociation is requested. No source flags are produced.
enum class NumericPolicy { native_float32 };
// Buffer ABI: input = [item_count, packed per-item words...], uniforms =
// packed uniform words, output = packed per-item words (NO status prefix).
// The caller must validate allocation extents and non-overlap before execution.
void emit_cpu_llvm(std::ostream&,const Kernel&,NumericPolicy);
void emit_spirv_llvm(std::ostream&,const Kernel&,NumericPolicy);
// Includes unresolved installation obligations; contains no source opcodes.
void write_manifest(std::ostream&,const Kernel&);
}
