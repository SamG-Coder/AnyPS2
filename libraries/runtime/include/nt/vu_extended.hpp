#pragma once
#include <nt/ee_float.hpp>
#if !defined(__SPIRV__)
#include <array>
#endif

namespace nt::numeric {
// EFU sine is a fixed polynomial, not the host libm sine. Keep each multiply
// and add separately rounded, including the progression of odd powers.
NT_SHADER_INLINE FiniteFloatResult sine_finite(uint32_t x) {
    const auto square=multiply_finite(x,x,true);
    auto power=x;
    auto sum=x;
    const uint32_t coefficients[]{0xbe2aaaa4U,0x3c08873eU,0xb94fb21fU,0x362e9c14U};
    // Fixed trip count; full unrolling keeps shader code free of private arrays.
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
    for (const auto coefficient:coefficients) {
        const auto raised=multiply_finite(power,square.bits,true);
        power=raised.bits;
        const auto term=multiply_finite(power,coefficient,true);
        const auto added=add_subtract_finite(sum,term.bits,false,true);
        sum=added.bits;
    }
    return {sum};
}
// Bounded finite EFU path, with explicit intermediate rounding. Reuse signed
// zero underflow from the arithmetic helpers; EFU does not publish MAC flags.
// Exponent 255 is an ordinary large value and overflow saturates, as in the
// unit's other arithmetic.
NT_SHADER_INLINE FiniteFloatResult efu_sum_squares_finite(uint32_t x,uint32_t y,uint32_t z) {
    uint32_t sum=0;
    const uint32_t components[]{x,y,z};
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
    for (const auto component:components) {
        const auto square=multiply_finite(component,component,true);
        const auto added=add_subtract_finite(sum,square.bits,false,true);
        sum=added.bits;
    }
    return {sum};
}
// The EFU reciprocal leaves a zero input unchanged rather than dividing by it.
NT_SHADER_INLINE FiniteFloatResult efu_reciprocal_finite(uint32_t x) {
    if (!(x&0x7f800000U)) return {x&0x80000000U};
    const auto reciprocal=divide_finite(0x3f800000,x,true);
    return {reciprocal.bits,false,reciprocal.flags==0};
}
NT_SHADER_INLINE FiniteFloatResult efu_length_finite(uint32_t x,uint32_t y,uint32_t z) {
    const auto sum=efu_sum_squares_finite(x,y,z);
    if (!sum.supported) return {0,false,false};
    const auto root=square_root_finite(sum.bits&0x7fffffffU,true);
    return {root.bits};
}
NT_SHADER_INLINE FiniteFloatResult efu_reciprocal_sum_squares_finite(uint32_t x,uint32_t y,uint32_t z) {
    const auto sum=efu_sum_squares_finite(x,y,z);
    return sum.supported ? efu_reciprocal_finite(sum.bits) : FiniteFloatResult{0,false,false};
}
NT_SHADER_INLINE FiniteFloatResult reciprocal_length_finite(uint32_t x,uint32_t y,uint32_t z) {
    const auto length=efu_length_finite(x,y,z);
    return length.supported ? efu_reciprocal_finite(length.bits) : FiniteFloatResult{0,false,false};
}
NT_SHADER_INLINE FiniteFloatResult efu_sum_finite(uint32_t x,uint32_t y,uint32_t z,uint32_t w) {
    uint32_t sum=x;
    const uint32_t components[]{y,z,w};
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
    for (const auto component:components) {
        const auto added=add_subtract_finite(sum,component,false,true);
        sum=added.bits;
    }
    return {sum};
}
// Like the EFU square root, this uses the magnitude of its input.
NT_SHADER_INLINE FiniteFloatResult efu_reciprocal_square_root_finite(uint32_t x) {
    return efu_reciprocal_finite(square_root_finite(x&0x7fffffffU,true).bits);
}
}
