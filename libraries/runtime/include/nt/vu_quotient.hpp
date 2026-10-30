#pragma once
#include <nt/ee_float.hpp>

namespace nt::numeric {
// Convert scalar exception flags to VU current invalid/divide flags. Sticky
// status is applied when the pending quotient becomes visible, not at issue.
inline ScalarFlagResult vu_divide_finite(std::uint32_t left, std::uint32_t right) {
    auto result = divide_finite(left, right);
    result.flags = (result.flags & 0x20040U) ? 0x10U : (result.flags & 0x10020U) ? 0x20U : 0U;
    return result;
}
inline ScalarFlagResult vu_square_root_finite(std::uint32_t right) {
    auto result = square_root_finite(right & 0x7fffffffU);
    result.flags = (right >> 31) && (right & 0x7f800000U) ? 0x10U : 0U;
    return result;
}
inline ScalarFlagResult vu_reciprocal_square_root_finite(std::uint32_t left, std::uint32_t right) {
    const auto root = vu_square_root_finite(right);
    if (!root.supported || ((left >> 23) & 255) == 255) return {0, 0, false};
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
