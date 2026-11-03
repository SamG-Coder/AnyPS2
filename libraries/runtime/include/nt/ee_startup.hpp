#pragma once
#include <cstdint>
#include <optional>
namespace nt::runtime {
struct InitialHeap { std::uint32_t begin,end; };
inline std::optional<InitialHeap> initial_heap(std::uint32_t begin,std::uint32_t size,std::uint32_t stack_base) {
    const auto end=size==0xffffffffU ? std::uint64_t(stack_base) : std::uint64_t(begin)+size;
    // SetupHeap records a boundary; it does not allocate or validate an arena.
    // The automatic-size form ignores begin and returns the main stack base.
    if (end>0xffffffffULL) return {};
    return InitialHeap{begin,static_cast<std::uint32_t>(end)};
}
struct InitialStack { std::uint32_t base,size,pointer; };
inline std::optional<InitialStack> initial_stack(std::uint32_t base,std::uint32_t size,std::uint64_t memory_size) {
    constexpr std::uint32_t reserve=0x2a0;
    if (memory_size<4096 || memory_size>0x2000000) return {};
    const auto top=base==0xffffffffU ? memory_size-4096 : std::uint64_t(base)+size;
    if (size<reserve || top<size || top>memory_size || top%16) return {};
    return InitialStack{static_cast<std::uint32_t>(top-size),size,static_cast<std::uint32_t>(top-reserve)};
}
}
