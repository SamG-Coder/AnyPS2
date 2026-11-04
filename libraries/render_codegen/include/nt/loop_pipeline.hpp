#pragma once
#include <nt/loop_structure.hpp>
#include <nt/element_abi.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nt::render {
// Lane-level expression graph of a loop's per-element computation under exact VU
// semantics. Operation nodes name VU operations, never host float operations;
// numeric lowering chooses an implementation later.
enum class ElementOperation : std::uint32_t {
    add, subtract, multiply, multiply_add, multiply_subtract, minimum, maximum, absolute,
    integer_to_float, float_to_integer, divide, square_root, reciprocal_square_root,
    reciprocal_length, sine, efu_square_root,
};
enum class ElementNodeKind : std::uint32_t {
    constant,   // value = bits
    invariant,  // register file value at loop entry that the loop never writes: a=file, b=index, c=lane
    load,       // a=address base (0-15 integer register, 16 absolute, 17 TOP, 18 ITOP), b=row (signed), c=lane
    operation,  // a=ElementOperation, b,c,d=operand node ids, value=scale for conversions
    entry,      // loop-carried state at entry (a=file, b=index, c=lane); never in a recovered function
};
// Register files for invariant nodes.
inline constexpr std::uint32_t vector_file=0,accumulator_file=1,immediate_file=2,quotient_file=3,p_file=4;
struct ElementNode {
    ElementNodeKind kind=ElementNodeKind::constant;
    std::uint32_t a=0,b=0,c=0,d=0,value=0;
    auto operator<=>(const ElementNode&) const = default;
};
struct ElementStore {
    unsigned pair=0,lane=0,base=0;  // base integer register of the output array
    std::int32_t row=0;             // output row relative to the element's base row
    std::int32_t stride=0;          // rows per element
    std::uint32_t node=0;
};
// Steady-state per-element function of a software-pipelined loop. Element k is
// the one stored by iteration k; every load and store row is relative to its base
// register's entry value plus that base's stride times k. The function is proven
// for iterations from depth on. Prologue and epilogue equivalence for the first
// and last elements, entry state and live-out registers are separate proofs.
// One flag-producing upper instruction (ADD/SUB/MUL/MADD/MSUB family) executed
// by the element's iteration: the node each written lane receives, including
// results that are never stored. They determine the status sticky bits.
inline constexpr std::uint32_t no_element_node=0xffffffffU;
struct ElementFlags {
    unsigned pair=0;
    std::array<std::uint32_t,4> lanes{no_element_node,no_element_node,no_element_node,no_element_node};
    bool accumulate=false;  // MADD/MSUB: product flags are also sticky
};
struct PipelinedLoop {
    unsigned depth=0;
    std::vector<ElementNode> nodes;
    std::vector<ElementStore> stores;
    std::array<std::int32_t,16> strides{};
    // Flag-producing instructions per iteration. Recovered when every such
    // instruction reaches the same entry-independent steady state as the stores;
    // otherwise flags is empty and flags_unrecovered explains why.
    std::vector<ElementFlags> flags;
    bool flag_instructions=false;
    std::string flags_unrecovered;
};
struct PipelineRecovery {
    std::optional<PipelinedLoop> loop;
    std::string reason;  // why recovery failed, empty on success
};
// Requires a structure without internal control flow, flag tests, kicks or
// non-affine/stationary memory. Simulates iterations to a bounded steady state.
PipelineRecovery recover_pipelined_loop(std::span<const vu::Pair> pairs,unsigned unit,const LoopStructure& structure);

// Element kernel ABI, shared by the CPU and Vulkan compute variants. Arithmetic
// calls the exact nt_gpu_element_* helpers, so both match the VU bit for bit.
//   data:       VU data memory snapshot, four words per row (read only)
//   parameters: element_parameters() layout (read only)
//   output:     per element, a status word (nonzero if an operand is one the VU
//               would stop on) followed by one word per ElementStore, in order
// Element item covers element first+item. Rows wrap at the unit's data size.
// Status word, one word per store, then a sticky-flag word (status bits 6-11
// produced by the element's iteration) when flags were recovered.
inline unsigned element_output_words(const PipelinedLoop& loop) {
    return 1+static_cast<unsigned>(loop.stores.size())+(loop.flags.empty() ? 0U : 1U);
}
// CPU entry: void nt_render_elements(const uint32_t* data,const uint32_t* parameters,uint32_t* output)
void emit_element_kernel_llvm(std::ostream&,const PipelinedLoop&,unsigned unit,bool vulkan,std::string_view symbol="nt_render_elements");
}
