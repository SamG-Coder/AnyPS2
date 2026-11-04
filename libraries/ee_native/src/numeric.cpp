#include <nt/ee_float.hpp>
#include <nt/ee_vector_math.hpp>
#include <nt/vu_quotient.hpp>
#include <nt/vu_extended.hpp>
#include <nt/vu_clip.hpp>
#include <bit>
// Converted code calls these per lane. Inline them into LLVM-optimized
// callers so straight-line lanes can combine; host builds keep ordinary calls.
#if defined(__clang__) && defined(NDEBUG)
#define NT_LANE_HELPER __attribute__((always_inline))
#else
#define NT_LANE_HELPER
#endif
namespace {
std::uint64_t pack(nt::numeric::FiniteFloatResult result) {
    return result.bits | (std::uint64_t(result.underflow)<<32) | (std::uint64_t(!result.supported)<<33) | (std::uint64_t(result.overflow)<<35) | (std::uint64_t(result.product_flags)<<36);
}
std::uint64_t pack_flags(nt::numeric::ScalarFlagResult result) {
    return result.bits | (std::uint64_t(result.flags)<<32) | (std::uint64_t(!result.supported)<<63);
}
// Host-float lane arithmetic for the common operand range. The product of two
// 24-bit significands has at most 48 bits and a sum of operands at most 24
// exponents apart fits 49 bits, so the host double result is exact. Clearing
// the low 29 double significand bits then chops to the 24-bit result, and the
// float conversion of that representable value is exact in every rounding mode.
// Operands that can reach exponent 255, overflow or underflow set
// exact_path_required; callers then use the integer implementations above.
constexpr std::uint64_t exact_path_required=1ULL<<62;
// One definition serves scalar lanes and, under Clang, four-lane vectors.
// Comparisons yield bool or lane masks; selects and bitwise logic work on both.
template<class U32> struct LaneTypes { using F32=float; using F64=double; using U64=std::uint64_t; };
NT_LANE_HELPER inline double to_double(float value) { return static_cast<double>(value); }
NT_LANE_HELPER inline float to_float(double value) { return static_cast<float>(value); }
#if defined(__clang__)
using U32x4=std::uint32_t __attribute__((ext_vector_type(4)));
using U32x8=std::uint32_t __attribute__((ext_vector_type(8)));
template<> struct LaneTypes<U32x4> {
    using F32=float __attribute__((ext_vector_type(4)));
    using F64=double __attribute__((ext_vector_type(4)));
    using U64=std::uint64_t __attribute__((ext_vector_type(4)));
};
NT_LANE_HELPER inline LaneTypes<U32x4>::F64 to_double(LaneTypes<U32x4>::F32 value) { return __builtin_convertvector(value,LaneTypes<U32x4>::F64); }
NT_LANE_HELPER inline LaneTypes<U32x4>::F32 to_float(LaneTypes<U32x4>::F64 value) { return __builtin_convertvector(value,LaneTypes<U32x4>::F32); }
#endif
template<class U32> struct FastLanes { U32 bits; U32 exact; };
template<class U32> NT_LANE_HELPER inline auto host_double(U32 bits) {
    return to_double(std::bit_cast<typename LaneTypes<U32>::F32>(bits));
}
template<class U32> NT_LANE_HELPER inline U32 chop_to_float(typename LaneTypes<U32>::F64 value) {
    using U64=typename LaneTypes<U32>::U64;
    const auto chopped=std::bit_cast<U64>(value)&U64(~((1ULL<<29)-1));
    return std::bit_cast<U32>(to_float(std::bit_cast<typename LaneTypes<U32>::F64>(chopped)));
}
// Lane masks are all ones for true. Scalar lanes use 1 and 0.
template<class U32,class Condition> NT_LANE_HELPER inline U32 lane_mask(Condition condition) {
    return condition ? U32(0xffffffffU) : U32(0U);
}
// Matches multiply_finite(..., true) where exact is set.
template<class U32> NT_LANE_HELPER inline FastLanes<U32> fast_multiply(U32 a,U32 b) {
    const U32 ea=(a>>23)&255U,eb=(b>>23)&255U;
    const U32 zero=lane_mask<U32>(ea==0U)|lane_mask<U32>(eb==0U);
    // Exponent-zero operands are signed zero; their bits must not reach the host as denormals.
    const U32 left=ea!=0U ? a : a&0x80000000U,right=eb!=0U ? b : b&0x80000000U;
    const U32 sum=ea+eb;
    const U32 exact=lane_mask<U32>(ea!=255U)&lane_mask<U32>(eb!=255U)&(zero|(lane_mask<U32>(sum>=128U)&lane_mask<U32>(sum<=380U)));
    return {chop_to_float<U32>(host_double(left)*host_double(right)),exact};
}
// Matches add_subtract_finite(..., true) where exact is set.
template<class U32> NT_LANE_HELPER inline FastLanes<U32> fast_add(U32 a,U32 b,std::uint32_t subtract) {
    const U32 signed_b=b^(subtract ? 0x80000000U : 0U);
    const U32 ea=(a>>23)&255U,eb=(b>>23)&255U;
    // Exponents 25..253 cannot cancel below the normal range or carry into 255.
    const U32 exact=(lane_mask<U32>(ea==0U)|(lane_mask<U32>(ea>=25U)&lane_mask<U32>(ea<=253U)))&
                    (lane_mask<U32>(eb==0U)|(lane_mask<U32>(eb>=25U)&lane_mask<U32>(eb<=253U)));
    const U32 left=ea!=0U ? a : a&0x80000000U,right=eb!=0U ? signed_b : signed_b&0x80000000U;
    const U32 sum=chop_to_float<U32>(host_double(left)+host_double(right));
    // Beyond 24 exponents an opposing addend chops to the preceding encoding;
    // a host double sum does not retain that sticky information.
    const U32 borrow=(a^signed_b)>>31;
    const U32 nonzero=lane_mask<U32>(ea!=0U)&lane_mask<U32>(eb!=0U);
    const U32 wide=ea>eb+24U ? a-borrow : signed_b-borrow;
    const U32 aligned=lane_mask<U32>(ea>eb+24U)|lane_mask<U32>(eb>ea+24U);
    return {(nonzero&aligned)!=0U ? wide : sum,exact};
}
// An exact product never underflows or overflows: its stage flags are Z and S.
template<class U32> NT_LANE_HELPER inline U32 product_flags(U32 product) {
    return ((product&0x7fffffffU)==0U ? U32(1U) : U32(0U))|((product>>31)<<1);
}
}
// Fixed-width C ABI keeps the LLVM caller independent of C++ struct-return rules.
extern "C" std::uint32_t nt_vu_clip_flags(std::uint32_t x,std::uint32_t y,std::uint32_t z,std::uint32_t w) {
    return nt::numeric::clip_flags(x,y,z,w);
}
extern "C" std::uint64_t nt_vu_reciprocal_length_finite(std::uint32_t x,std::uint32_t y,std::uint32_t z) {
    return pack(nt::numeric::reciprocal_length_finite(x,y,z));
}
extern "C" std::uint64_t nt_vu_sine_finite(std::uint32_t x) {
    return pack(nt::numeric::sine_finite(x));
}
extern "C" std::uint64_t nt_vu_efu_square_root_finite(std::uint32_t x) {
    const auto root=nt::numeric::square_root_finite(x&0x7fffffffU);
    return pack(nt::numeric::FiniteFloatResult{root.bits,false,root.supported && root.flags==0});
}
extern "C" std::uint64_t nt_vu_divide_finite(std::uint32_t left,std::uint32_t right) {
    return pack_flags(nt::numeric::vu_divide_finite(left,right));
}
extern "C" std::uint64_t nt_vu_square_root_finite(std::uint32_t right) {
    return pack_flags(nt::numeric::vu_square_root_finite(right));
}
extern "C" std::uint64_t nt_vu_reciprocal_square_root_finite(std::uint32_t left,std::uint32_t right) {
    return pack_flags(nt::numeric::vu_reciprocal_square_root_finite(left,right));
}
extern "C" std::uint64_t nt_ee_add_finite(std::uint32_t left,std::uint32_t right,std::uint32_t subtract) {
    return pack(nt::numeric::add_subtract_finite(left,right,subtract!=0,true));
}

extern "C" std::uint64_t nt_ee_multiply_finite(std::uint32_t left,std::uint32_t right) {
    return pack(nt::numeric::multiply_finite(left,right,true));
}

// EE scalar arithmetic uses exponent 255 as finite, unlike IEEE float. Keep
// the scalar accumulator contract separate from the VU helpers above.
extern "C" std::uint64_t nt_ee_scalar_add(std::uint32_t left,std::uint32_t right,std::uint32_t subtract) {
    return pack(nt::numeric::add_subtract_finite(left,right,subtract!=0,true));
}
extern "C" std::uint64_t nt_ee_scalar_multiply(std::uint32_t left,std::uint32_t right) {
    return pack(nt::numeric::multiply_finite(left,right,true));
}

extern "C" std::uint64_t nt_ee_divide_finite(std::uint32_t left,std::uint32_t right) {
    return pack_flags(nt::numeric::divide_finite(left,right,true));
}
extern "C" std::uint32_t nt_ee_integer_to_scalar(std::uint32_t value) {
    return nt::numeric::integer_to_scalar(value);
}
extern "C" std::uint32_t nt_ee_scalar_to_integer(std::uint32_t value) {
    return nt::numeric::scalar_to_integer(value);
}
extern "C" NT_LANE_HELPER std::uint32_t nt_ee_min_max_scalar(std::uint32_t left,std::uint32_t right,std::uint32_t maximum) {
    return nt::numeric::min_max_scalar(left,right,maximum!=0);
}

extern "C" NT_LANE_HELPER std::uint32_t nt_ee_integer_to_scaled_scalar(std::uint32_t value,std::uint32_t scale) {
    return nt::numeric::integer_to_scaled_scalar(value,scale);
}
extern "C" NT_LANE_HELPER std::uint32_t nt_ee_scaled_scalar_to_integer(std::uint32_t value,std::uint32_t scale) {
    return nt::numeric::scaled_scalar_to_integer(value,scale);
}

extern "C" std::uint64_t nt_ee_square_root_finite(std::uint32_t value) {
    return pack_flags(nt::numeric::square_root_finite(value,true));
}

extern "C" NT_LANE_HELPER std::uint32_t nt_ee_vector_lane_flags(std::uint64_t packed,std::uint32_t lane) {
    return lane<4 ? nt::numeric::vector_lane_flags(static_cast<std::uint32_t>(packed),((packed>>32)&1)!=0,((packed>>35)&1)!=0,lane) : 0;
}
extern "C" NT_LANE_HELPER std::uint32_t nt_ee_vector_status_flags(std::uint32_t mac,std::uint32_t previous) {
    return nt::numeric::vector_status_flags(mac,previous);
}

extern "C" std::uint64_t nt_ee_multiply_accumulate_finite(std::uint32_t accumulator,std::uint32_t left,
                                                       std::uint32_t right,std::uint32_t subtract) {
    return pack(nt::numeric::vector_multiply_accumulate(accumulator,left,right,subtract!=0));
}

extern "C" std::uint64_t nt_ee_scalar_multiply_accumulate(std::uint32_t accumulator,std::uint32_t left,
                                                              std::uint32_t right,std::uint32_t subtract,
                                                              std::uint32_t accumulator_overflow) {
    const auto product=nt::numeric::multiply_finite(left,right,true);
    // A product overflow takes precedence over a prior accumulator overflow.
    // MSUB reverses the product sign before that saturated result is propagated.
    if (product.overflow)
        return pack({product.bits^(subtract ? 0x80000000U : 0U),false,true,true});
    if (accumulator_overflow)
        return pack({accumulator|0x7fffffffU,false,true,true}) | (std::uint64_t(product.underflow)<<34);
    const auto result=nt::numeric::add_subtract_finite(accumulator,product.bits,subtract!=0,true);
    // Bit 34 preserves intermediate sticky underflow; bit 35 is final overflow.
    return pack(result)|(std::uint64_t(product.underflow)<<34);
}

extern "C" std::uint64_t nt_ee_reciprocal_square_root_finite(std::uint32_t left,std::uint32_t right) {
    // Preserve the bounded denominator contract. Exceptional denominator semantics
    // require separate validation rather than a fabricated quotient.
    if ((right>>31) || !(right&0x7f800000U)) return pack_flags({0,0,false});
    const auto root=nt::numeric::square_root_finite(right,true);
    if (!root.supported) return pack_flags(root);
    return pack_flags(nt::numeric::divide_finite(left,root.bits,true));
}

// Exact-when-flagged lane forms of nt_ee_add_finite, nt_ee_multiply_finite and
// nt_ee_multiply_accumulate_finite. Branch-free so converted VU code can
// evaluate all lanes and take the exact integer path once per block.
extern "C" NT_LANE_HELPER std::uint64_t nt_vu_add_fast(std::uint32_t left,std::uint32_t right,std::uint32_t subtract) {
    const auto lane=fast_add(left,right,subtract);
    return lane.bits|(lane.exact ? 0 : exact_path_required);
}
extern "C" NT_LANE_HELPER std::uint64_t nt_vu_multiply_fast(std::uint32_t left,std::uint32_t right) {
    const auto lane=fast_multiply(left,right);
    return lane.bits|(lane.exact ? 0 : exact_path_required);
}
extern "C" NT_LANE_HELPER std::uint64_t nt_vu_multiply_accumulate_fast(std::uint32_t accumulator,std::uint32_t left,
                                                       std::uint32_t right,std::uint32_t subtract) {
    const auto product=fast_multiply(left,right);
    const auto sum=fast_add(accumulator,product.bits,subtract);
    return sum.bits|(std::uint64_t(product_flags(product.bits))<<36)|(product.exact && sum.exact ? 0 : exact_path_required);
}
#if defined(__clang__)
// Four-lane forms for converted VU code. Lanes 0-3 hold results; lanes 4-7
// hold bit 0 = exact path required and bits 4-5 = MADD/MSUB product Z/S.
// Callers pass zero for unwritten lanes. These exist only in LLVM bitcode.
namespace {
NT_LANE_HELPER inline U32x8 vector_lanes(FastLanes<U32x4> result,U32x4 product) {
    const U32x4 info=(~result.exact&1U)|(product<<4);
    return __builtin_shufflevector(result.bits,info,0,1,2,3,4,5,6,7);
}
}
extern "C" NT_LANE_HELPER U32x8 nt_vu_vector_add_fast(U32x4 left,U32x4 right,std::uint32_t subtract) {
    return vector_lanes(fast_add(left,right,subtract),U32x4(0U));
}
extern "C" NT_LANE_HELPER U32x8 nt_vu_vector_multiply_fast(U32x4 left,U32x4 right) {
    return vector_lanes(fast_multiply(left,right),U32x4(0U));
}
extern "C" NT_LANE_HELPER U32x8 nt_vu_vector_multiply_accumulate_fast(U32x4 accumulator,U32x4 left,U32x4 right,std::uint32_t subtract) {
    const auto product=fast_multiply(left,right);
    auto sum=fast_add(accumulator,product.bits,subtract);
    sum.exact&=product.exact;
    return vector_lanes(sum,product_flags(product.bits));
}
#endif
