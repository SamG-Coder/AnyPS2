#pragma once
#include <stdint.h>

namespace nt::numeric {
// Bit-pattern comparisons preserve signed zero and denormal threshold behavior
// without depending on the host floating-point environment.
inline uint32_t clip_flags(uint32_t x,uint32_t y,uint32_t z,uint32_t w) {
    const auto limit=(w&0x7f800000U) ? w&0x7fffffffU : 0x007fffffU;
    const uint32_t values[3]{x,y,z};
    uint32_t flags=0;
    for (unsigned lane=0;lane<3;++lane)
        // Bit 2*lane (+) or 2*lane+1 (-), written as a shift of 1 or 2 so the
        // optimizer does not form a funnel shift the SPIR-V backend rejects.
        if ((values[lane]&0x7fffffffU)>limit) flags|=(1U+(values[lane]>>31))<<(lane*2);
    return flags;
}
}
