#pragma once
#include <nt/ee_float.hpp>
#include <array>

namespace nt::numeric {
using Vector=std::array<std::uint32_t,4>;
struct VectorResult { Vector value; std::uint32_t mac,status; bool supported=true; };
inline std::uint32_t vector_lane_flags(std::uint32_t bits,bool underflow,unsigned lane) {
    const auto bit=1U<<(3-lane);
    return (!(bits&0x7fffffff) ? bit : 0U)|((bits>>31) ? bit<<4 : 0U)|(underflow ? bit<<8 : 0U);
}
inline std::uint32_t vector_status_flags(std::uint32_t mac,std::uint32_t previous) {
    const auto current=((mac&15) ? 1U : 0U)|((mac&0xf0) ? 2U : 0U)|((mac&0xf00) ? 4U : 0U);
    return (previous&0xff0)|current|(current<<6);
}

template<class Arithmetic>
inline VectorResult binary_vector(Vector original,Vector left,Vector right,unsigned mask,
                                  unsigned broadcast,std::uint32_t previous_status,Arithmetic arithmetic) {
    VectorResult result{original,0,previous_status};
    if (mask>15 || broadcast>4) { result.supported=false; return result; }
    for (unsigned lane=0;lane<4;++lane) {
        const auto bit=1U<<(3-lane);
        if (!(mask&bit)) continue;
        const auto value=arithmetic(left[lane],right[broadcast<4 ? broadcast : lane]);
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
        [subtract](auto a,auto b) { return add_subtract_finite(a,b,subtract); });
}
inline VectorResult multiply_vector(Vector original,Vector left,Vector right,unsigned mask,
                                    unsigned broadcast,std::uint32_t previous_status) {
    return binary_vector(original,left,right,mask,broadcast,previous_status,multiply_finite);
}
}
