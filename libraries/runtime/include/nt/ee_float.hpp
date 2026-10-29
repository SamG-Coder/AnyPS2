#pragma once
#include <algorithm>
#include <bit>
#include <cstdint>

namespace nt::numeric {
struct FiniteFloatResult {
    std::uint32_t bits;
    bool underflow=false;
    bool supported=true;
};
inline std::uint64_t shift_jam(std::uint64_t value, unsigned shift) {
    if (!shift) return value;
    if (shift >= 64) return value != 0;
    return (value >> shift) | ((value & ((1ULL << shift)-1)) != 0);
}
inline FiniteFloatResult add_subtract_finite(std::uint32_t a, std::uint32_t b, bool subtract) {
    const unsigned ea = (a >> 23) & 255, eb = (b >> 23) & 255;
    if (ea == 255 || eb == 255)
        return {0,false,false};
    const unsigned exponent = std::max(ea,eb);
    // Extra fraction bits plus a sticky bit avoid host rounding mode dependence.
    const auto magnitude = [exponent](std::uint32_t bits, unsigned e) {
        return e ? shift_jam(std::uint64_t((bits & 0x7fffff) | 0x800000) << 32,exponent-e) : 0ULL;
    };
    const auto x = magnitude(a,ea), y = magnitude(b,eb);
    const bool sx = (a >> 31) != 0, sy = ((b >> 31) != 0) != subtract;
    bool sign;
    std::uint64_t value;
    if (sx == sy) { value = x+y; sign = sx; }
    else if (x >= y) { value = x-y; sign = sx; }
    else { value = y-x; sign = sy; }
    if (!value) return {(!x && !y && sx && sy) ? 0x80000000U : 0U};
    const unsigned high = 63U - static_cast<unsigned>(std::countl_zero(value));
    const int result_exponent = static_cast<int>(exponent) + static_cast<int>(high) - 55;
    const auto sign_bit = sign ? 0x80000000U : 0U;
    if (result_exponent <= 0) return {sign_bit,true};
    if (result_exponent >= 255)
        return {0,false,false};
    const auto significand = high >= 23 ? value >> (high-23) : value << (23-high);
    return {sign_bit | (static_cast<std::uint32_t>(result_exponent) << 23) |
            (static_cast<std::uint32_t>(significand) & 0x7fffff)};
}
inline FiniteFloatResult multiply_finite(std::uint32_t a,std::uint32_t b) {
    const auto ea=(a>>23)&255,eb=(b>>23)&255;
    if (ea==255 || eb==255) return {0,false,false};
    const auto sign=(a^b)&0x80000000U;
    if (!ea || !eb) return {sign};
    // Exact 24-bit significand product followed by explicit truncation.
    const auto product=std::uint64_t((a&0x7fffff)|0x800000)*((b&0x7fffff)|0x800000);
    const unsigned high=(product&(1ULL<<47)) ? 47 : 46;
    const int exponent=int(ea+eb)-127+int(high)-46;
    if (exponent>=255) return {0,false,false};
    if (exponent<=0) return {sign,true};
    return {sign|(std::uint32_t(exponent)<<23)|(std::uint32_t(product>>(high-23))&0x7fffff)};
}

inline std::uint32_t integer_to_scalar(std::uint32_t bits) {
    const auto sign = bits & 0x80000000U;
    const auto magnitude = sign ? 0U-bits : bits;
    if (!magnitude) return 0;
    const unsigned high = 31U-static_cast<unsigned>(std::countl_zero(magnitude));
    const auto fraction = high > 23 ? magnitude >> (high-23) : magnitude << (23-high);
    return sign | ((high+127)<<23) | (fraction & 0x7fffff);
}
inline std::uint32_t scalar_to_integer(std::uint32_t bits) {
    const auto exponent=(bits>>23)&255;
    const bool negative=(bits>>31)!=0;
    if (exponent<127) return 0;
    if (exponent>=158) return negative ? 0x80000000U : 0x7fffffffU;
    const auto significand=(bits&0x7fffff)|0x800000;
    const auto magnitude=exponent>=150 ? significand << (exponent-150) : significand >> (150-exponent);
    return negative ? 0U-magnitude : magnitude;
}
inline std::uint32_t min_max_scalar(std::uint32_t a, std::uint32_t b, bool maximum) {
    const auto key=[](std::uint32_t value) { return (value>>31) ? ~value : value|0x80000000U; };
    return (maximum ? key(a)>key(b) : key(a)<key(b)) ? a : b;
}

// VU fixed-point conversions use the same chop and saturation policy, with
// an exact power-of-two scale. No host float casts or ambient rounding mode.
inline std::uint32_t integer_to_scaled_scalar(std::uint32_t bits,unsigned scale) {
    const auto converted=integer_to_scalar(bits);
    return converted ? converted-(scale<<23) : 0;
}
inline std::uint32_t scaled_scalar_to_integer(std::uint32_t bits,unsigned scale) {
    const auto exponent=(bits>>23)&255;
    if (!exponent) return 0;
    if (exponent+scale>=158) return bits>>31 ? 0x80000000U : 0x7fffffffU;
    return scalar_to_integer(bits+(scale<<23));
}
struct ScalarFlagResult { std::uint32_t bits, flags; bool supported=true; };
inline ScalarFlagResult divide_finite(std::uint32_t a,std::uint32_t b) {
    const auto ea=(a>>23)&255, eb=(b>>23)&255;
    if (ea==255 || eb==255) return {0,0,false};
    const auto sign=(a^b)&0x80000000U;
    auto result=sign, flags=0U;
    if (!eb) {
        result |= 0x7f7fffff;
        flags |= ea ? 0x10020 : 0x20040;
    } else if (ea) {
        const auto numerator=(a&0x7fffff)|0x800000, denominator=(b&0x7fffff)|0x800000;
        const bool normalize=numerator<denominator;
        int exponent=static_cast<int>(ea)-static_cast<int>(eb)+127-(normalize ? 1 : 0);
        const auto scaled=std::uint64_t(numerator) << (normalize ? 24 : 23);
        auto quotient=scaled/denominator;
        const auto remainder=scaled%denominator;
        // Division uses nearest-even independently of the ordinary chop mode.
        if (remainder*2>denominator || (remainder*2==denominator && (quotient&1))) ++quotient;
        if (quotient==0x1000000) { quotient>>=1; ++exponent; }
        if (exponent>=255) return {0,0,false};
        if (exponent>0) result |= (static_cast<std::uint32_t>(exponent)<<23) |
                                 (static_cast<std::uint32_t>(quotient)&0x7fffff);
    }
    return {result,flags};
}
inline ScalarFlagResult square_root_finite(std::uint32_t bits) {
    const auto exponent=(bits>>23)&255;
    if (exponent==255) return {0,0,false};
    if (!exponent) return {bits&0x80000000U,0};
    const auto significand=(bits&0x7fffffU)|0x800000U;
    // The normalized radicand is an exact integer with at most 48 bits.
    // Build its integer square root without relying on host FP rounding.
    const auto radicand=std::uint64_t(significand)<<(exponent&1 ? 23 : 24);
    std::uint64_t root=0;
    for (std::uint64_t bit=1ULL<<23;bit;bit>>=1) {
        const auto candidate=root|bit;
        if (candidate*candidate<=radicand) root=candidate;
    }
    // sqrt(N) rounds above the midpoint iff N-root^2 > root. There is
    // no exact half-integer square root for an integer N, hence no tie.
    if (radicand-root*root>root) ++root;
    auto output_exponent=(exponent+127)/2;
    if (root==0x1000000) { root>>=1; ++output_exponent; }
    const auto value=(output_exponent<<23)|(std::uint32_t(root)&0x7fffffU);
    return {value,bits>>31 ? 0x20040U : 0U};
}
}
