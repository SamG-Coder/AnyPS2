#pragma once
#include <nt/ee_float.hpp>
#include <array>

namespace nt::numeric {
using Vector=std::array<std::uint32_t,4>;
struct VectorResult { Vector value; std::uint32_t mac,status; bool supported=true; };
inline std::uint32_t vector_lane_flags(std::uint32_t bits,bool underflow,unsigned lane) {
    const auto bit=1U<<(3-lane);
    return (!(bits&0x7fffffff) ? bit : 0U)|((bits>>31) ? bit<<4 : 0U)|(underflow ? bit<<8 : 0U)|
           (((bits>>23)&255)==255 ? bit<<12 : 0U);
}
inline std::uint32_t vector_status_flags(std::uint32_t mac,std::uint32_t previous) {
    const auto current=((mac&15) ? 1U : 0U)|((mac&0xf0) ? 2U : 0U)|((mac&0xf00) ? 4U : 0U)|((mac&0xf000) ? 8U : 0U);
    return (previous&0xff0)|current|(current<<6);
}

// The VU and scalar units share the finite encoded range, but not the
// accumulator overflow policy. Keep unsupported intermediate cases explicit.
inline FiniteFloatResult vector_multiply_accumulate(std::uint32_t accumulator,std::uint32_t left,
                                                    std::uint32_t right,bool subtract) {
    const auto product=multiply_finite(left,right,true);
    if (product.underflow) return {0,false,false};
    const auto signed_product=product.bits^(subtract ? 0x80000000U : 0U);
    if (product.overflow) {
        if ((accumulator&0x7f800000U) && ((accumulator^signed_product)&0x80000000U))
            return {0,false,false};
        return {signed_product,false,true,true};
    }
    return add_subtract_finite(accumulator,product.bits,subtract,true);
}

template<class Arithmetic>
NT_FORCE_INLINE VectorResult binary_vector(Vector original,Vector left,Vector right,unsigned mask,
                                  unsigned broadcast,std::uint32_t previous_status,Arithmetic arithmetic) {
    VectorResult result{original,0,previous_status};
    if (mask>15 || broadcast>4) { result.supported=false; return result; }
    for (unsigned lane=0;lane<4;++lane) {
        const auto bit=1U<<(3-lane);
        if (!(mask&bit)) continue;
        const auto value=arithmetic(left[lane],right[broadcast<4 ? broadcast : lane],lane);
        if (!value.supported) { result.supported=false; return result; }
        result.value[lane]=value.bits;
        result.mac|=vector_lane_flags(value.bits,value.underflow,lane);
    }
    result.status=vector_status_flags(result.mac,previous_status);
    return result;
}
inline VectorResult add_subtract_vector(Vector original,Vector left,Vector right,unsigned mask,
                                        bool subtract,unsigned broadcast,std::uint32_t previous_status) {
    return binary_vector(original,left,right,mask,broadcast,previous_status,
        [subtract](auto a,auto b,unsigned) { return add_subtract_finite(a,b,subtract,true); });
}
inline VectorResult multiply_vector(Vector original,Vector left,Vector right,unsigned mask,
                                    unsigned broadcast,std::uint32_t previous_status) {
    return binary_vector(original,left,right,mask,broadcast,previous_status,[](auto a,auto b,unsigned) { return multiply_finite(a,b,true); });
}
}
