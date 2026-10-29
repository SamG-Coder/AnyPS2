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

}
