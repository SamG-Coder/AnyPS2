#pragma once
#include <nt/ee_float.hpp>

namespace nt::numeric {
// The VU has no infinities or NaNs: exponent 255 reads as the largest normal
// magnitude of its sign. Games can reach this with uninitialized vector input.
NT_SHADER_INLINE uint32_t vu_quotient_operand(uint32_t bits) {
    return ((bits >> 23) & 255) == 255 ? (bits & 0x80000000U) | 0x7f7fffffU : bits;
}
// Convert scalar exception flags to VU current invalid/divide flags. Sticky
// status is applied when the pending quotient becomes visible, not at issue.
NT_SHADER_INLINE ScalarFlagResult vu_divide_finite(uint32_t left, uint32_t right) {
    const auto a = vu_quotient_operand(left), b = vu_quotient_operand(right);
    auto result = divide_finite(a, b);
    // Both operands are finite here, so a refusal can only be overflow. Q
    // saturates, and these operations have no overflow flag to report it.
    if (!result.supported) return {((a ^ b) & 0x80000000U) | 0x7f7fffffU, 0U};
    result.flags = (result.flags & 0x20040U) ? 0x10U : (result.flags & 0x10020U) ? 0x20U : 0U;
    return result;
}
NT_SHADER_INLINE ScalarFlagResult vu_square_root_finite(uint32_t right) {
    auto result = square_root_finite(vu_quotient_operand(right) & 0x7fffffffU);
    result.flags = (right >> 31) && (right & 0x7f800000U) ? 0x10U : 0U;
    return result;
}
NT_SHADER_INLINE ScalarFlagResult vu_reciprocal_square_root_finite(uint32_t left, uint32_t right) {
    const auto root = vu_square_root_finite(right);
    if (!root.supported) return {0, 0, false};
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
