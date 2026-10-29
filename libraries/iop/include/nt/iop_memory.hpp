#pragma once
#include <cstdint>
#include <cstddef>
#include <optional>

namespace nt::iop {
inline std::optional<std::uint32_t> ram_offset(std::uint32_t address,unsigned size,std::size_t ram_size) {
    const auto segment=address&0xe0000000U;
    if (segment==0x80000000U || segment==0xa0000000U) address&=0x1fffffff;
    if (!size || address%size || address>=2*1024*1024 || size>2*1024*1024-address ||
        address>ram_size || size>ram_size-address) return {};
    return address;
}
}
