#pragma once
#include <nt/compiler_attributes.hpp>
#include <stdint.h>
#if !defined(__clang__) && !defined(__GNUC__)
#include <bit>
#endif

// Shared, freestanding arithmetic for native CPU code and LLVM GPU kernels.
// Keep the encoded range, truncation and error flags identical on both targets.
namespace nt::numeric {
inline unsigned highest_bit(uint64_t nonzero) {
#if defined(__SPIRV__)
    // The Vulkan LLVM target cannot yet legalize ctlz. This bounded integer
    // search has the same nonzero-input contract and requires no extension.
    unsigned bit=0;
    if (nonzero>>32) { nonzero>>=32; bit+=32; }
    if (nonzero>>16) { nonzero>>=16; bit+=16; }
    if (nonzero>>8) { nonzero>>=8; bit+=8; }
    if (nonzero>>4) { nonzero>>=4; bit+=4; }
    if (nonzero>>2) { nonzero>>=2; bit+=2; }
    return bit+static_cast<unsigned>(nonzero>>1);
#elif defined(__clang__) || defined(__GNUC__)
    return 63U-static_cast<unsigned>(__builtin_clzll(nonzero));
#else
    return 63U-static_cast<unsigned>(std::countl_zero(nonzero));
#endif
}
struct FiniteFloatResult {
    uint32_t bits;
    bool underflow=false;
    bool supported=true;
    bool overflow=false;
    uint32_t product_flags=0; // VU multiply stage Z/S/U/O; separate from final flags.
};
inline uint64_t shift_jam(uint64_t value, unsigned shift) {
    if (!shift) return value;
    if (shift >= 64) return value != 0;
    return (value >> shift) | ((value & ((1ULL << shift)-1)) != 0);
}
NT_FORCE_INLINE FiniteFloatResult add_subtract_finite(uint32_t a, uint32_t b, bool subtract, bool extended=false) {
    const unsigned ea = (a >> 23) & 255, eb = (b >> 23) & 255;
    if (!extended && (ea == 255 || eb == 255))
        return {0,false,false};
    const auto signed_b=b^(subtract ? 0x80000000U : 0U);
    // Exponent-zero inputs are signed zero on this interface, including denormals.
    if (!ea) return {!eb ? (a&signed_b&0x80000000U) : signed_b};
    if (!eb) return {a};
    // Beyond the 24-bit significand, adding a smaller magnitude cannot carry.
    // Subtracting any nonzero amount truncates to the preceding encoded value,
    // including the exponent boundary. The larger exponent is at least 26 here,
    // so this shortcut cannot underflow. Keep the tie/cancellation range below.
    if (ea>eb+24) return {a-((a^signed_b)>>31)};
    if (eb>ea+24) return {signed_b-((a^signed_b)>>31)};
    const unsigned exponent = (ea < eb ? ea : eb);
    // The remaining exponent distance is at most 24. Align to the smaller
    // exponent: both complete significands fit in 48 bits, so cancellation is
    // exact and no discarded-bit/sticky calculation is needed.
    const auto x = uint64_t((a&0x7fffff)|0x800000) << (ea-exponent);
    const auto y = uint64_t((b&0x7fffff)|0x800000) << (eb-exponent);
    const bool sx = (a >> 31) != 0, sy = ((b >> 31) != 0) != subtract;
    bool sign;
    uint64_t value;
    if (sx == sy) { value = x+y; sign = sx; }
    else if (x >= y) { value = x-y; sign = sx; }
    else { value = y-x; sign = sy; }
    if (!value) return {(!x && !y && sx && sy) ? 0x80000000U : 0U};
    const unsigned high = highest_bit(value);
    const int result_exponent = static_cast<int>(exponent) + static_cast<int>(high) - 23;
    const auto sign_bit = sign ? 0x80000000U : 0U;
    if (result_exponent <= 0) return {sign_bit,true};
    if (result_exponent >= (extended ? 256 : 255))
        return extended ? FiniteFloatResult{sign_bit|0x7fffffffU,false,true,true} : FiniteFloatResult{0,false,false};
    const auto significand = high >= 23 ? value >> (high-23) : value << (23-high);
    return {sign_bit | (static_cast<uint32_t>(result_exponent) << 23) |
            (static_cast<uint32_t>(significand) & 0x7fffff)};
}
NT_FORCE_INLINE FiniteFloatResult multiply_finite(uint32_t a,uint32_t b,bool extended=false) {
    const auto ea=(a>>23)&255,eb=(b>>23)&255;
    if (!extended && (ea==255 || eb==255)) return {0,false,false};
    const auto sign=(a^b)&0x80000000U;
    if (!ea || !eb) return {sign};
    // Exact 24-bit significand product followed by explicit truncation.
    const auto product=uint64_t((a&0x7fffff)|0x800000)*((b&0x7fffff)|0x800000);
    const unsigned high=(product&(1ULL<<47)) ? 47 : 46;
    const int exponent=int(ea+eb)-127+int(high)-46;
    if (exponent>=(extended ? 256 : 255))
        return extended ? FiniteFloatResult{sign|0x7fffffffU,false,true,true} : FiniteFloatResult{0,false,false};
    if (exponent<=0) return {sign,true};
    return {sign|(uint32_t(exponent)<<23)|(uint32_t(product>>(high-23))&0x7fffff)};
}

}
