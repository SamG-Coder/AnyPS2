#pragma once
#include <nt/render_loop.hpp>
#include <array>
#include <stdexcept>

namespace render_fixture {
struct Configuration {
    std::array<unsigned,4> columns;
    unsigned vertex,output,uniform_base,input_base,output_base,count;
    unsigned input_stride,output_stride,destination;
    int uniform_offset,input_offset,output_offset;
};
inline Configuration configuration(unsigned variant) {
    if (variant==0) return {{1,2,3,4},5,6,3,1,2,4,1,1,0,0,0,0};
    if (variant==1) return {{9,12,15,17},20,23,10,6,8,7,3,2,112,-2,2,1};
    throw std::invalid_argument("Unknown original loop fixture");
}
inline std::uint32_t upper(unsigned function,unsigned destination,unsigned left,unsigned right,unsigned mask=15) {
    return (mask<<21)|(right<<16)|(left<<11)|(destination<<6)|function;
}
constexpr std::uint32_t upper_nop=0x000002ff,lower_nop=0x8000033c;
inline void word(nt::Bytes& bytes,std::uint32_t value) {
    for (unsigned n=0;n<4;++n) bytes.push_back(static_cast<std::uint8_t>(value>>(n*8)));
}
inline void replace(nt::Bytes& bytes,unsigned pair,bool is_upper,std::uint32_t value) {
    for (unsigned n=0;n<4;++n) bytes.at(pair*8+(is_upper ? 4 : 0)+n)=static_cast<std::uint8_t>(value>>(n*8));
}
inline std::uint32_t load(unsigned vector,unsigned base,int offset) {
    return (15U<<21)|(vector<<16)|(base<<11)|(static_cast<unsigned>(offset)&0x7ffU);
}
inline std::uint32_t store(unsigned vector,unsigned base,int offset) {
    return (1U<<25)|(15U<<21)|(base<<16)|(vector<<11)|(static_cast<unsigned>(offset)&0x7ffU);
}
inline std::uint32_t add(unsigned index,int amount) {
    return 0x80000032U|(index<<16)|(index<<11)|((static_cast<unsigned>(amount)&31U)<<6);
}
inline nt::Bytes program(unsigned variant) {
    // Original synthetic loop. No commercial program or SDK bytes are included.
    const auto c=configuration(variant);
    nt::Bytes bytes;
    const auto pair=[&](std::uint32_t lower=lower_nop,std::uint32_t high=upper_nop) {
        word(bytes,lower); word(bytes,high);
    };
    for (unsigned n=0;n<4;++n) pair(load(c.columns[n],c.uniform_base,c.uniform_offset+static_cast<int>(n)));
    pair(load(c.vertex,c.input_base,c.input_offset)); pair();
    pair(lower_nop,upper(60,6,c.columns[0],c.vertex));
    pair(lower_nop,upper(61,2,c.columns[1],c.vertex));
    pair(lower_nop,upper(62,2,c.columns[2],c.vertex));
    pair(lower_nop,upper(11,c.output,c.columns[3],c.vertex));
    pair(store(c.output,c.output_base,c.output_offset));
    pair(add(c.input_base,static_cast<int>(c.input_stride)));
    pair(add(c.output_base,static_cast<int>(c.output_stride)));
    pair(add(c.count,-1)); pair(); pair();
    const auto displacement=-static_cast<int>(bytes.size()/8)-1;
    pair((41U<<25)|(c.count<<11)|(static_cast<unsigned>(displacement)&0x7ffU)); pair();
    return bytes;
}
inline nt::render::VertexLoop loop(unsigned variant,unsigned unit=1) {
    const auto c=configuration(variant);
    return nt::render::analyze_vertex_loop(nt::vu::frame(program(variant),unit,c.destination),unit);
}
inline std::array<std::uint16_t,16> integers(unsigned variant,unsigned count,unsigned output_row=600) {
    const auto c=configuration(variant);
    std::array<std::uint16_t,16> result{};
    result[c.uniform_base]=static_cast<std::uint16_t>((variant ? 8 : 0)-c.uniform_offset);
    result[c.input_base]=static_cast<std::uint16_t>(16-c.input_offset);
    result[c.output_base]=static_cast<std::uint16_t>(static_cast<int>(output_row)-c.output_offset);
    result[c.count]=static_cast<std::uint16_t>(count);
    return result;
}
}
