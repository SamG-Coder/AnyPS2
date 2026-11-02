#pragma once
#include <nt/vulkan_renderer.hpp>
#include <algorithm>
#include <optional>
#include <stdexcept>

namespace nt::graphics {
inline std::optional<Sprite> plan_sprite(std::uint64_t frame,std::uint64_t offset,std::uint64_t scissor,
    unsigned x0,unsigned y0,unsigned x1,unsigned y1,std::uint32_t color) {
    const unsigned format=(frame>>24)&63, pitch=(frame>>16)&63;
    if (format>1 || !pitch) throw std::invalid_argument("Unsupported sprite surface format or zero pitch");
    const auto ceil_pixel=[](int value) { return value>=0 ? (value+15)/16 : value/16; };
    const int ox=offset&65535,oy=(offset>>32)&65535;
    const int left=std::max(ceil_pixel(int(std::min(x0,x1))-ox),int(scissor&2047));
    const int right=std::min(ceil_pixel(int(std::max(x0,x1))-ox),int((scissor>>16)&2047)+1);
    const int top=std::max(ceil_pixel(int(std::min(y0,y1))-oy),int((scissor>>32)&2047));
    const int bottom=std::min(ceil_pixel(int(std::max(y0,y1))-oy),int((scissor>>48)&2047)+1);
    const auto mask=std::uint32_t(frame>>32)|(format==1 ? 0xff000000U : 0U);
    // A fully masked color buffer may still receive depth writes.
    if (left>=right || top>=bottom) return {};
    return Sprite{unsigned(left),unsigned(top),unsigned(right),unsigned(bottom),unsigned(frame&511)*32,pitch,color,mask};
}
}
