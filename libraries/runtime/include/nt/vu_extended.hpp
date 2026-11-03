#pragma once
#include <nt/ee_float.hpp>
#include <array>

namespace nt::numeric {
// EFU sine is a fixed polynomial, not the host libm sine. Keep each multiply
// and add separately rounded, including the progression of odd powers.
inline FiniteFloatResult sine_finite(std::uint32_t x) {
    const auto square=multiply_finite(x,x);
    if (!square.supported || square.overflow) return {0,false,false};
    auto power=x;
    auto sum=x;
    for (const auto coefficient:std::array{0xbe2aaaa4U,0x3c08873eU,0xb94fb21fU,0x362e9c14U}) {
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
inline FiniteFloatResult reciprocal_length_finite(std::uint32_t x,std::uint32_t y,std::uint32_t z) {
    std::uint32_t sum=0;
    for (const auto component:std::array{x,y,z}) {
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
