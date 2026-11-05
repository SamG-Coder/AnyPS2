#pragma once
#include <nt/vulkan_renderer.hpp>
#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>

namespace nt::graphics {
// Target fields of a FRAME register, with zero bounds and color. Vector
// batches use this directly; their bounds are planned per primitive on the GPU.
inline Sprite sprite_target(std::uint64_t frame) {
    const unsigned format=(frame>>24)&63, pitch=(frame>>16)&63;
    // Z32 (0x30) and Z24 (0x31) frames receive color in the depth page layout.
    const bool depth_layout=format==0x30 || format==0x31;
    if ((format>1 && format!=2 && format!=10 && !depth_layout) || !pitch) throw std::invalid_argument("Unsupported drawing surface: format="+
        std::to_string(format)+" pitch="+std::to_string(pitch)+" frame="+std::to_string(frame));
    const auto mask=std::uint32_t(frame>>32)|(format==1 || format==0x31 ? 0xff000000U : 0U);
    Sprite result{0,0,0,0,unsigned(frame&511)*32,pitch,0,mask};
    if (format==2 || format==10) result.flags|=color_16_flag|(format==10 ? color_16s_flag : 0);
    if (depth_layout) result.flags|=color_z_flag;
    return result;
}
// vector_assembly.comp repeats these bounds for GPU-produced triangles.
inline std::optional<Sprite> plan_sprite(std::uint64_t frame,std::uint64_t offset,std::uint64_t scissor,
    unsigned x0,unsigned y0,unsigned x1,unsigned y1,std::uint32_t color) {
    auto result=sprite_target(frame);
    const auto ceil_pixel=[](int value) { return value>=0 ? (value+15)/16 : value/16; };
    const int ox=offset&65535,oy=(offset>>32)&65535;
    const int left=std::max(ceil_pixel(int(std::min(x0,x1))-ox),int(scissor&2047));
    const int right=std::min(ceil_pixel(int(std::max(x0,x1))-ox),int((scissor>>16)&2047)+1);
    const int top=std::max(ceil_pixel(int(std::min(y0,y1))-oy),int((scissor>>32)&2047));
    const int bottom=std::min(ceil_pixel(int(std::max(y0,y1))-oy),int((scissor>>48)&2047)+1);
    // A fully masked color buffer may still receive depth writes.
    if (left>=right || top>=bottom) return {};
    result.left=unsigned(left); result.top=unsigned(top); result.right=unsigned(right); result.bottom=unsigned(bottom);
    result.color=color;
    return result;
}
}
