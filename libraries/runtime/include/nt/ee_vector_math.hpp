#pragma once
#include <nt/ee_float.hpp>
#include <array>

namespace nt::numeric {
using Vector=std::array<std::uint32_t,4>;
struct VectorResult { Vector value; std::uint32_t mac,status; bool supported=true; };
inline std::uint32_t vector_lane_flags(std::uint32_t bits,bool underflow,bool overflow,unsigned lane) {
    const auto bit=1U<<(3-lane);
    return (!(bits&0x7fffffff) ? bit : 0U)|((bits>>31) ? bit<<4 : 0U)|(underflow ? bit<<8 : 0U)|
           (overflow ? bit<<12 : 0U);
}
inline std::uint32_t vector_status_flags(std::uint32_t mac,std::uint32_t previous) {
    // Reduce each four-lane flag group to one bit in parallel. Adding seven to
    // its low three bits sets the high bit exactly when those bits are nonzero;
    // OR retains an already-set high bit. No carry crosses a nibble boundary.
    const auto nonzero=(((mac&0x7777U)+0x7777U)|mac)&0x8888U;
    // Gather bits 3,7,11,15 into bits 0..3. The four shifted contributions
    // selected by this multiplication do not overlap or introduce carries.
    const auto current=((nonzero*0x249U)>>12)&15U;
    return (previous&0xff0)|current|(current<<6);
}

// The VU and scalar units share the finite encoded range, but not the
// accumulator overflow policy. A product overflow takes precedence over the
// accumulator; it cannot be cancelled by adding a finite opposing value.
inline FiniteFloatResult vector_multiply_accumulate(std::uint32_t accumulator,std::uint32_t left,
                                                    std::uint32_t right,bool subtract) {
    const auto product=multiply_finite(left,right,true);
    const auto product_flags=vector_status_flags(vector_lane_flags(product.bits,product.underflow,product.overflow,0),0)&15U;
    const auto signed_product=product.bits^(subtract ? 0x80000000U : 0U);
    if (product.overflow) {
        return {signed_product,false,true,true,product_flags};
    }
    auto result=add_subtract_finite(accumulator,product.bits,subtract,true);
    result.product_flags=product_flags;
    return result;
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
        result.status|=value.product_flags<<6;
        result.mac|=vector_lane_flags(value.bits,value.underflow,value.overflow,lane);
    }
    result.status=vector_status_flags(result.mac,result.status);
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
