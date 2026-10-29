#pragma once
#include <cstdint>

namespace nt::runtime {
inline constexpr std::uint32_t uncached_ram_begin=0x20100000U;
inline constexpr std::uint32_t uncached_ram_end=0x22000000U;
inline constexpr std::uint32_t accelerated_ram_begin=0x30100000U;
inline constexpr std::uint32_t accelerated_ram_end=0x32000000U;
// Native application RAM layout. The first MiB is intentionally excluded from
// user uncached aliases. This does not install arbitrary application TLB mappings.
constexpr std::uint32_t default_ram_address(std::uint32_t address) {
    if ((address>=uncached_ram_begin && address<uncached_ram_end) ||
        (address>=accelerated_ram_begin && address<accelerated_ram_end)) return address&0x0fffffffU;
    if (address>=0x80000000U && address<0xc0000000U) return address&0x1fffffffU;
    return address;
}
}
