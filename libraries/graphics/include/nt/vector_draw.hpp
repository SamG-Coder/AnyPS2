#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace nt::graphics {
// GPU-produced PACKED vertex batches. A recovered VU element kernel computes
// one vertex record per element on the GPU; a renderer-side assembly pass turns
// the records into native draw records without a host round trip. The host
// keeps GS register state and plans everything that does not depend on vertex
// data. Only the subset documented in render-loop-lifting.md is accepted.

// Store-word index meaning "use the constant word instead".
inline constexpr std::uint32_t vector_constant_word=0xffffffffU;
// One GS register of the PACKED vertex record. Each of its four qword words is
// either element store word store[n] or constant[n].
struct VectorRegister {
    unsigned reg=0;
    std::array<std::uint32_t,4> store{vector_constant_word,vector_constant_word,vector_constant_word,vector_constant_word};
    std::array<std::uint32_t,4> constant{};
};
// A write of VU data memory at a row, applied on the GPU before the element
// kernel runs, in order. Resident writes are content-identified streams kept on
// the device and uploaded only when their content is new; other writes are
// staged per launch. Both have the same effect on the kernel's memory view.
struct VectorMemoryWrite {
    unsigned row=0;
    std::span<const std::uint32_t> words;  // four words per row
    bool resident=false;
};
// ABI of an AOT-generated launch-range guard. No runtime instruction analysis.
enum class VectorRangeStatus : std::uint32_t { valid=0, count=1, startup=2, wrap=3, alias=4 };
using VectorRangeGuard=std::uint32_t (*)(const std::uint32_t*);
// One element-kernel launch and the PACKED vertex layout of its output.
struct VectorLaunch {
    unsigned kernel=0;                      // VulkanRenderer::load_vector_kernel result
    std::span<const VectorMemoryWrite> writes;
    std::span<const std::uint32_t> parameters;  // nt::render::element_parameters() words
    unsigned elements=0;                    // one vertex per element
    unsigned element_words=0;               // status word plus store words per element
    std::span<const VectorRegister> vertex; // registers of one vertex, XYZ register last
    VectorRangeGuard range_guard=nullptr; // optional for explicit low-level kernels; required by recovered regions
    std::span<const std::uint32_t> prefix; // startup records: status plus store words
};
// Host-planned, data-independent part of primitive assembly. Field order is
// the GPU plan layout in vector_assembly.comp.
struct VectorAssembly {
    std::uint32_t primitive=0;              // 3 list, 4 strip, 5 fan
    std::uint32_t shaded=0,coverage=0,xyz_register=0;
    std::uint32_t present=0;                // bit n: register kind n is in the record
    std::int32_t offset_x=0,offset_y=0;     // XYOFFSET in 12.4 units
    std::array<std::uint32_t,4> scissor{};  // x0, x1, y0, y1 in pixels
    std::array<std::array<std::uint32_t,4>,5> store{};     // per kind and word
    std::array<std::array<std::uint32_t,4>,5> constant{};
    std::array<std::uint32_t,6> state{};    // color, q, s, t, uv, fog when not in the record
};
// Register kinds of VectorAssembly::store/constant/present.
enum class VectorRegisterKind : unsigned { color=0, texture=1, uv=2, fog=3, position=4 };
inline constexpr unsigned vector_primitives(unsigned primitive,unsigned vertices) {
    return primitive==3 ? vertices/3 : vertices>=2 ? vertices-2 : 0;
}
}
