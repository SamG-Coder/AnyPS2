#pragma once
#include <nt/ee_float.hpp>

namespace nt::numeric {
// Convert scalar exception flags to VU current invalid/divide flags. Sticky
// status is applied when the pending quotient becomes visible, not at issue.
// The unit has no infinity or NaN: exponent 255 is an ordinary large value, and a
// result beyond the format saturates. A zero divisor still gives the 0x7f7fffff
// magnitude that a program can observe.
NT_SHADER_INLINE ScalarFlagResult vu_divide_finite(uint32_t left, uint32_t right) {
    auto result = divide_finite(left, right, true);
    if (!(right & 0x7f800000U) && (left & 0x7f800000U)) result.bits = (result.bits & 0x80000000U) | 0x7f7fffffU;
    result.flags = (result.flags & 0x20040U) ? 0x10U : (result.flags & 0x10020U) ? 0x20U : 0U;
    return result;
}
NT_SHADER_INLINE ScalarFlagResult vu_square_root_finite(uint32_t right) {
    auto result = square_root_finite(right & 0x7fffffffU, true);
    result.flags = (right >> 31) && (right & 0x7f800000U) ? 0x10U : 0U;
    return result;
}
NT_SHADER_INLINE ScalarFlagResult vu_reciprocal_square_root_finite(uint32_t left, uint32_t right) {
    const auto root = vu_square_root_finite(right);
    if (!(right & 0x7f800000U)) {
        const auto sign = (left ^ right) & 0x80000000U;
        return (left & 0x7f800000U) ? ScalarFlagResult{sign | 0x7f7fffffU, 0x20U} :
            ScalarFlagResult{sign, 0x30U};
    }
    auto result = vu_divide_finite(left, root.bits);
    result.flags |= root.flags;
    return result;
}
}
