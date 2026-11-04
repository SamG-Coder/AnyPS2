#pragma once
#include <nt/vu_native.hpp>
#include <nt/element_abi.hpp>
#include <array>
#include <span>

namespace nt::render {
using RegionIteration=vu::Result (*)(vu::State*);
using RegionKernel=void (*)(const std::uint32_t*,const std::uint32_t*,std::uint32_t*);
using RegionGuard=std::uint32_t (*)(const std::uint32_t*);
struct RegionStore { std::uint32_t base=0; std::int32_t row=0,stride=0; std::uint32_t lane=0; };
// Immutable compiler output. Function pointers are native linker relocations.
// State is the existing migration ABI; no instruction words or dispatch table.
struct VectorRegion {
    std::uint32_t unit=0,head=0,exit=0,startup=0,drain=0,counter=0,step=0,stores=0;
    std::array<std::int32_t,16> strides{};
    std::array<RegionStore,64> output{};
    RegionIteration iteration=nullptr;
    RegionGuard guard=nullptr;
    RegionKernel kernel=nullptr;
};
// Optional synchronous executor for the same AOT element kernel. It must finish
// all writes before returning, report failures, and not mutate input buffers.
// Device ownership remains with the caller; this interface promises no speedup.
using RegionExecutor=void (*)(void*,std::span<const std::uint32_t>,std::span<const std::uint32_t>,std::span<std::uint32_t>);
struct RegionExecution { unsigned startup=0,elements=0,drain=0; };
struct PreparedRegion {
    vu::State state;
    ElementEntry entry;
    std::vector<std::uint32_t> parameters;
    RegionExecution execution;
};
// Validate before reading entry memory. Preparation reconstructs registers and
// genuine startup stores but leaves steady outputs for the selected executor.
std::vector<std::uint32_t> validate_vector_region(const VectorRegion&,const vu::State&);
PreparedRegion prepare_vector_region(const VectorRegion&,const vu::State&);
unsigned region_output_row(const VectorRegion&,const ElementEntry&,unsigned store,unsigned element);
// A region entry prepared for a device-resident element kernel. prepared.state
// is the complete exit state except the steady-state output rows, which the
// device produces. A graphics consumer draws them from the device; any other
// consumer must first call materialize_region_outputs, which computes them with
// the same AOT kernel and yields exactly the execute_vector_region result.
struct RegionLaunch {
    PreparedRegion prepared;
    std::vector<std::uint32_t> memory;      // kernel input: entry data memory
    std::vector<std::uint32_t> parameters;  // steady elements, written after the prefix
    std::vector<std::uint32_t> prefix;      // startup elements: zero status then store words
    unsigned elements=0;                    // startup plus steady elements
};
RegionLaunch prepare_region_launch(const VectorRegion&,const vu::State&);
void materialize_region_outputs(const VectorRegion&,const RegionLaunch&,vu::State&,RegionExecutor=nullptr,void* context=nullptr);
// Transactional: unsupported entry/count/range, boundary or kernel failures
// leave the caller's state unchanged. No interpretation or runtime compilation.
RegionExecution execute_vector_region(const VectorRegion&,vu::State&,RegionExecutor=nullptr,void* context=nullptr);
}
