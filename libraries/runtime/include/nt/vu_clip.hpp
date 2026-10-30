#pragma once
#include <array>
#include <cstdint>

namespace nt::numeric {
// Bit-pattern comparisons preserve signed zero and denormal threshold behavior
// without depending on the host floating-point environment.
inline std::uint32_t clip_flags(std::uint32_t x,std::uint32_t y,std::uint32_t z,std::uint32_t w) {
    const auto limit=(w&0x7f800000U) ? w&0x7fffffffU : 0x007fffffU;
    std::uint32_t flags=0;
    unsigned lane=0;
    for (const auto value:std::array{x,y,z}) {
        if ((value&0x7fffffffU)>limit) flags|=1U<<(lane*2+(value>>31));
        ++lane;
    }
    return flags;
}
}
