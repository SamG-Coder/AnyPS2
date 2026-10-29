#pragma once
#include <nt/ee_float.hpp>
#include <array>

namespace nt::numeric {
using Vector=std::array<std::uint32_t,4>;
struct VectorResult { Vector value; std::uint32_t mac,status; bool supported=true; };
inline VectorResult add_subtract_vector(Vector original,Vector left,Vector right,unsigned mask,
                                        bool subtract,unsigned broadcast,std::uint32_t previous_status) {
    VectorResult result{original,0,previous_status};
    if (mask>15 || broadcast>4) { result.supported=false; return result; }
    for (unsigned lane=0;lane<4;++lane) {
        const auto bit=1U<<(3-lane);
        if (!(mask&bit)) continue;
        const auto value=add_subtract_finite(left[lane],right[broadcast<4 ? broadcast : lane],subtract);
        if (!value.supported) { result.supported=false; return result; }
        result.value[lane]=value.bits;
        if (!(value.bits&0x7fffffff)) result.mac|=bit;
        if (value.bits>>31) result.mac|=bit<<4;
        if (value.underflow) result.mac|=bit<<8;
    }
    const auto current=((result.mac&15) ? 1U : 0U)|((result.mac&0xf0) ? 2U : 0U)|((result.mac&0xf00) ? 4U : 0U);
    result.status=(previous_status&0xff0)|current|(current<<6);
    return result;
}
}
