#pragma once
#include <nt/vu_native.hpp>
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
// Transactional: unsupported entry/count/range, boundary or kernel failures
// leave the caller's state unchanged. No interpretation or runtime compilation.
RegionExecution execute_vector_region(const VectorRegion&,vu::State&,RegionExecutor=nullptr,void* context=nullptr);
}
