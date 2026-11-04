#pragma once
#include <nt/ee_float.hpp>
#if !defined(__SPIRV__)
#include <array>
#endif

namespace nt::numeric {
// EFU sine is a fixed polynomial, not the host libm sine. Keep each multiply
// and add separately rounded, including the progression of odd powers.
NT_SHADER_INLINE FiniteFloatResult sine_finite(uint32_t x) {
    const auto square=multiply_finite(x,x);
    if (!square.supported || square.overflow) return {0,false,false};
    auto power=x;
    auto sum=x;
    const uint32_t coefficients[]{0xbe2aaaa4U,0x3c08873eU,0xb94fb21fU,0x362e9c14U};
    // Fixed trip count; full unrolling keeps shader code free of private arrays.
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
    for (const auto coefficient:coefficients) {
        const auto raised=multiply_finite(power,square.bits);
        if (!raised.supported || raised.overflow) return {0,false,false};
        power=raised.bits;
        const auto term=multiply_finite(power,coefficient);
        if (!term.supported || term.overflow) return {0,false,false};
        const auto added=add_subtract_finite(sum,term.bits,false);
        if (!added.supported || added.overflow) return {0,false,false};
        sum=added.bits;
    }
    return {sum};
}
// Bounded finite EFU path, with explicit intermediate rounding. Reuse signed
// zero underflow from the arithmetic helpers; EFU does not publish MAC flags.
// Intermediate overflow remains an explicit unsupported boundary.
NT_SHADER_INLINE FiniteFloatResult reciprocal_length_finite(uint32_t x,uint32_t y,uint32_t z) {
    uint32_t sum=0;
    const uint32_t components[]{x,y,z};
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
    for (const auto component:components) {
        const auto square=multiply_finite(component,component);
        if (!square.supported || square.overflow) return {0,false,false};
        const auto added=add_subtract_finite(sum,square.bits,false);
        if (!added.supported || added.overflow) return {0,false,false};
        sum=added.bits;
    }
    const auto root=square_root_finite(sum);
    if (!root.supported) return {0,false,false};
    if (!root.bits) return {0};
    const auto reciprocal=divide_finite(0x3f800000,root.bits);
    return {reciprocal.bits,false,reciprocal.supported && reciprocal.flags==0};
}
}
