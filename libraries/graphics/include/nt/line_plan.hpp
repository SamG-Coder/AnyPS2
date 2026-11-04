#pragma once
#include <nt/sprite_plan.hpp>

namespace nt::graphics {
// Points and lines use nearest-pixel positions; triangles and rectangles use
// top/left inclusion instead. Keep signed coordinates until after clipping.
inline int nearest_pixel(int coordinate) {
    const int value=coordinate+8;
    return value>=0 ? value/16 : (value-15)/16;
}
inline std::optional<PlannedPrimitive> plan_line_or_point(std::uint64_t frame,std::uint64_t offset,
    std::uint64_t scissor,std::array<RasterVertex,2> vertices,bool point,bool shaded,bool antialiased=false) {
    for (auto& v:vertices) {
        if (v.x<0 || v.x>65535 || v.y<0 || v.y>65535)
            throw std::invalid_argument("Invalid native point/line vertex");
        v.x-=int(offset&65535); v.y-=int((offset>>32)&65535);
    }
    const auto& a=vertices[0]; const auto& b=vertices[1];
    if (!point && a.x==b.x && a.y==b.y) return {};
    const int border=antialiased && !point ? 1 : 0;
    const int left=std::min(nearest_pixel(a.x),nearest_pixel(b.x))-border;
    const int top=std::min(nearest_pixel(a.y),nearest_pixel(b.y))-border;
    const int right=std::max(nearest_pixel(a.x),nearest_pixel(b.x))+1+border;
    const int bottom=std::max(nearest_pixel(a.y),nearest_pixel(b.y))+1+border;
    if (right<=0 || bottom<=0) return {};
    auto bounds=plan_sprite(frame,0,scissor,unsigned(std::max(left,0)*16),unsigned(std::max(top,0)*16),
        unsigned(right*16),unsigned(bottom*16),b.color);
    if (!bounds) return {};
    bounds->depth=b.z;
    if (!point) bounds->flags|=line_flag|(shaded ? shaded_triangle_flag : 0);
    return PlannedPrimitive{*bounds,{a,b,{}}};
}
}
