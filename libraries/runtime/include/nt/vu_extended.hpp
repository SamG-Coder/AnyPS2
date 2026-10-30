#pragma once
#include <nt/ee_float.hpp>
#include <array>

namespace nt::numeric {
// Bounded finite EFU path, with explicit intermediate rounding. Underflow and
// overflow that need additional EFU semantics remain unsupported.
inline FiniteFloatResult reciprocal_length_finite(std::uint32_t x,std::uint32_t y,std::uint32_t z) {
    std::uint32_t sum=0;
    for (const auto component:std::array{x,y,z}) {
        const auto square=multiply_finite(component,component);
        if (!square.supported || square.underflow) return {0,false,false};
        const auto added=add_subtract_finite(sum,square.bits,false);
        if (!added.supported || added.underflow) return {0,false,false};
        sum=added.bits;
    }
    const auto root=square_root_finite(sum);
    if (!root.supported) return {0,false,false};
    if (!root.bits) return {0};
    const auto reciprocal=divide_finite(0x3f800000,root.bits);
    return {reciprocal.bits,false,reciprocal.supported && reciprocal.flags==0};
}
}
