#pragma once
#include <nt/ee_vector_math.hpp>
#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#include <immintrin.h>
#endif

namespace nt::ee::detail {
enum class VectorArithmetic { add,subtract,multiply,multiply_add,multiply_subtract };

inline bool has_explicit_vector_rounding() {
#if defined(_MSC_VER) && defined(_M_X64)
    static const bool available=[] {
        int registers[4]{};
        __cpuid(registers,0);
        if (registers[0]<7) return false;
        __cpuidex(registers,1,0);
        if ((registers[2]&0x18000000)!=0x18000000 || (_xgetbv(0)&0xe6)!=0xe6) return false;
        __cpuidex(registers,7,0);
        return (registers[1]&(1<<16))!=0;
    }();
    return available;
#else
    return false;
#endif
}

#if defined(_MSC_VER) && defined(_M_X64)
// Isolated from baseline x64 code: only called after CPU and OS-state checks.
// Embedded chop rounding and suppressed host exceptions leave MXCSR untouched.
NT_FORCE_INLINE numeric::Vector rounded_vector(numeric::Vector left,numeric::Vector right,bool multiply) {
    const auto a=_mm512_castsi512_ps(_mm512_maskz_loadu_epi32(15,left.data()));
    const auto b=_mm512_castsi512_ps(_mm512_maskz_loadu_epi32(15,right.data()));
    const auto value=multiply ? _mm512_maskz_mul_round_ps(15,a,b,_MM_FROUND_TO_ZERO|_MM_FROUND_NO_EXC) :
        _mm512_maskz_add_round_ps(15,a,b,_MM_FROUND_TO_ZERO|_MM_FROUND_NO_EXC);
    numeric::Vector result;
    _mm512_mask_storeu_epi32(result.data(),15,_mm512_castps_si512(value));
    return result;
}
#endif

#if defined(_MSC_VER) && defined(_M_X64)
__declspec(noinline) inline bool vector_arithmetic_avx512(numeric::VectorResult& result,numeric::Vector left,numeric::Vector right,
        const numeric::Vector& accumulator,unsigned mask,unsigned broadcast,std::uint32_t status,VectorArithmetic operation) {

    const bool multiply=operation>=VectorArithmetic::multiply;
    const bool accumulate=operation>=VectorArithmetic::multiply_add;
    const bool subtract=operation==VectorArithmetic::subtract || operation==VectorArithmetic::multiply_subtract;
    const auto normal_or_zero=[](std::uint32_t& bits) {
        const auto exponent=(bits>>23)&255;
        if (!exponent) { bits&=0x80000000U; return true; }
        // A cancellation of two 24-bit significands cannot underflow in this
        // range, and a same-sign sum cannot reach the extended exponent range.
        return exponent>=25 && exponent<=253;
    };
    if (broadcast<4) right.fill(right[broadcast]);
    for (unsigned lane=0;lane<4;++lane) {
        if (!(mask&(8U>>lane))) { left[lane]=right[lane]=0; continue; }
        if (multiply) {
            const auto ea=(left[lane]>>23)&255,eb=(right[lane]>>23)&255;
            if (!ea) left[lane]&=0x80000000U;
            if (!eb) right[lane]&=0x80000000U;
            if (ea==255 || eb==255) return false;
            // Either exact zero, or a product whose rounded exponent is 1..254.
            if (ea && eb && (ea+eb<128 || ea+eb>380)) return false;
        } else {
            if (!normal_or_zero(left[lane]) || !normal_or_zero(right[lane])) return false;
            if (subtract) right[lane]^=0x80000000U;
        }
    }
    auto value=rounded_vector(left,right,multiply);
    if (accumulate) {
        auto addend=accumulator;
        for (unsigned lane=0;lane<4;++lane) {
            if (!(mask&(8U>>lane))) { value[lane]=addend[lane]=0; continue; }
            if (!normal_or_zero(value[lane]) || !normal_or_zero(addend[lane])) return false;
            if (subtract) value[lane]^=0x80000000U;
        }
        // Deliberately separate product and sum rounding; never a fused FMA.
        value=rounded_vector(addend,value,false);
    }
    result.mac=0;
    for (unsigned lane=0;lane<4;++lane) if (mask&(8U>>lane)) {
        result.value[lane]=value[lane];
        result.mac|=numeric::vector_lane_flags(value[lane],false,lane);
    }
    result.status=numeric::vector_status_flags(result.mac,status);
    return true;
}
#endif
inline bool try_vector_arithmetic(numeric::VectorResult& result,numeric::Vector left,numeric::Vector right,
        const numeric::Vector& accumulator,unsigned mask,unsigned broadcast,std::uint32_t status,VectorArithmetic operation) {
#if defined(_MSC_VER) && defined(_M_X64)
    return has_explicit_vector_rounding() && vector_arithmetic_avx512(result,left,right,accumulator,mask,broadcast,status,operation);
#else
    return false;
#endif
}
}
