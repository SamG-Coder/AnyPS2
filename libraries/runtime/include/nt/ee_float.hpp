#pragma once
#include <nt/finite_arithmetic.hpp>
#if !defined(__SPIRV__)
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#endif

namespace nt::numeric {
// Separate product and accumulation rounding; never use a host fused operation.
// Intermediate underflow needs additional VU pipeline semantics and is rejected.
NT_SHADER_INLINE FiniteFloatResult multiply_accumulate_finite(uint32_t accumulator,uint32_t left,
                                                   uint32_t right,bool subtract) {
    const auto product=multiply_finite(left,right);
    if (!product.supported || product.underflow) return {0,false,false};
    return add_subtract_finite(accumulator,product.bits,subtract);
}

// Portable integer definitions. Shader code uses them directly; host forms below
// must equal them for every input (tests compare the two).
inline uint32_t integer_to_scalar_portable(uint32_t bits) {
    const auto sign=bits&0x80000000U;
    const auto magnitude=sign ? 0U-bits : bits;
    if (!magnitude) return 0;
    const unsigned high=highest_bit(magnitude);
    const auto fraction=high>23 ? magnitude>>(high-23) : magnitude<<(23-high);
    return sign|((high+127)<<23)|(fraction&0x7fffff);
}
inline uint32_t scalar_to_integer_portable(uint32_t bits) {
    const auto exponent=(bits>>23)&255;
    const bool negative=(bits>>31)!=0;
    if (exponent<127) return 0;
    if (exponent>=158) return negative ? 0x80000000U : 0x7fffffffU;
    const auto significand=(bits&0x7fffff)|0x800000;
    const auto magnitude=exponent>=150 ? significand<<(exponent-150) : significand>>(150-exponent);
    return negative ? 0U-magnitude : magnitude;
}
// floor(sqrt(radicand)) for radicands below 2^48, by a bit-serial search.
inline uint64_t integer_square_root_portable(uint64_t radicand) {
    uint64_t root=0;
    // A fixed 24-step search; full unrolling leaves shaders a structured CFG.
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
    for (uint64_t bit=1ULL<<23;bit;bit>>=1) {
        const auto candidate=root|bit;
        if (candidate*candidate<=radicand) root=candidate;
    }
    return root;
}
#if defined(__SPIRV__)
inline uint32_t integer_to_scalar(uint32_t bits) { return integer_to_scalar_portable(bits); }
inline uint32_t scalar_to_integer(uint32_t bits) { return scalar_to_integer_portable(bits); }
#else
// A 32-bit integer converts exactly to a host double. Clearing the low 29
// double significand bits chops it to 24 bits, and the float conversion of
// that representable value is exact in every host rounding mode.
inline uint32_t integer_to_scalar(uint32_t bits) {
    const auto exact=std::bit_cast<uint64_t>(static_cast<double>(static_cast<int32_t>(bits)));
    return std::bit_cast<uint32_t>(static_cast<float>(std::bit_cast<double>(exact&~((1ULL<<29)-1))));
}
// Chop toward zero and saturate. Exponents 127..157 are below 2^31 in magnitude,
// where the host truncating conversion is exact; other ranges are selected.
inline uint32_t scalar_to_integer(uint32_t bits) {
    const auto exponent=(bits>>23)&255;
    const bool negative=(bits>>31)!=0;
    const auto in_range=exponent<158 ? bits : 0U;
    const auto truncated=static_cast<uint32_t>(static_cast<int32_t>(std::bit_cast<float>(in_range)));
    const auto saturated=negative ? 0x80000000U : 0x7fffffffU;
    return exponent<127 ? 0U : exponent>=158 ? saturated : truncated;
}
#endif
inline uint32_t min_max_scalar(uint32_t a, uint32_t b, bool maximum) {
    const auto key=[](uint32_t value) { return (value>>31) ? ~value : value|0x80000000U; };
    return (maximum ? key(a)>key(b) : key(a)<key(b)) ? a : b;
}

// VU fixed-point conversions use the same chop and saturation policy, with
// an exact power-of-two scale. No host float casts or ambient rounding mode.
inline uint32_t integer_to_scaled_scalar(uint32_t bits,unsigned scale) {
    const auto converted=integer_to_scalar(bits);
    return converted ? converted-(scale<<23) : 0;
}
inline uint32_t scaled_scalar_to_integer(uint32_t bits,unsigned scale) {
    const auto exponent=(bits>>23)&255;
    // Exponent zero is signed zero; a scaled exponent at or beyond 158 saturates.
    const auto saturated=bits>>31 ? 0x80000000U : 0x7fffffffU;
    const auto converted=scalar_to_integer(exponent && exponent+scale<158 ? bits+(scale<<23) : 0U);
    return !exponent ? 0U : exponent+scale>=158 ? saturated : converted;
}
struct ScalarFlagResult { uint32_t bits, flags; bool supported=true; };
NT_SHADER_INLINE ScalarFlagResult divide_finite(uint32_t a,uint32_t b,bool extended=false) {
    const auto ea=(a>>23)&255, eb=(b>>23)&255;
    if (!extended && (ea==255 || eb==255)) return {0,0,false};
    const auto sign=(a^b)&0x80000000U;
    auto result=sign, flags=0U;
    if (!eb) {
        result |= extended ? 0x7fffffffU : 0x7f7fffffU;
        flags |= ea ? 0x10020 : 0x20040;
    } else if (ea) {
        const auto numerator=(a&0x7fffff)|0x800000, denominator=(b&0x7fffff)|0x800000;
        const bool normalize=numerator<denominator;
        int exponent=static_cast<int>(ea)-static_cast<int>(eb)+127-(normalize ? 1 : 0);
        const auto scaled=uint64_t(numerator) << (normalize ? 24 : 23);
        auto quotient=scaled/denominator;
        const auto remainder=scaled%denominator;
        // Division uses nearest-even independently of the ordinary chop mode.
        if (remainder*2>denominator || (remainder*2==denominator && (quotient&1))) ++quotient;
        if (quotient==0x1000000) { quotient>>=1; ++exponent; }
        if (exponent>=(extended ? 256 : 255))
            return extended ? ScalarFlagResult{sign|0x7fffffffU,0} : ScalarFlagResult{0,0,false};
        if (exponent>0) result |= (static_cast<uint32_t>(exponent)<<23) |
                                 (static_cast<uint32_t>(quotient)&0x7fffff);
    }
    return {result,flags};
}
NT_SHADER_INLINE ScalarFlagResult square_root_finite(uint32_t bits,bool extended=false) {
    const auto exponent=(bits>>23)&255;
    if (!extended && exponent==255) return {0,0,false};
    if (!exponent) return {bits&0x80000000U,0};
    const auto significand=(bits&0x7fffffU)|0x800000U;
    // The normalized radicand is an exact integer with at most 48 bits, so its
    // host double conversion is exact. The host square root only proposes the
    // integer root: the integer corrections establish floor(sqrt(N)) in any
    // host rounding mode, without a bit-serial search.
    const auto radicand=uint64_t(significand)<<(exponent&1 ? 23 : 24);
#if defined(__SPIRV__)
    auto root=integer_square_root_portable(radicand);
#else
    auto root=static_cast<uint64_t>(std::sqrt(static_cast<double>(radicand)));
    while (root*root>radicand) --root;
    while ((root+1)*(root+1)<=radicand) ++root;
#endif
    // sqrt(N) rounds above the midpoint iff N-root^2 > root. There is
    // no exact half-integer square root for an integer N, hence no tie.
    if (radicand-root*root>root) ++root;
    auto output_exponent=(exponent+127)/2;
    if (root==0x1000000) { root>>=1; ++output_exponent; }
    const auto value=(output_exponent<<23)|(uint32_t(root)&0x7fffffU);
    return {value,bits>>31 ? 0x20040U : 0U};
}
}
