#pragma once
#include <nt/sprite_plan.hpp>

namespace nt::graphics {
inline std::optional<SpriteCommand> plan_triangle(std::uint64_t frame,std::uint64_t offset,
    std::uint64_t scissor,std::array<RasterVertex,3> vertices,bool shaded) {
    for (const auto& vertex:vertices)
        if (vertex.x<0 || vertex.x>65535 || vertex.y<0 || vertex.y>65535)
            throw std::invalid_argument("Invalid native triangle vertex");
    const auto [left,right]=std::minmax({vertices[0].x,vertices[1].x,vertices[2].x});
    const auto [top,bottom]=std::minmax({vertices[0].y,vertices[1].y,vertices[2].y});
    auto bounds=plan_sprite(frame,offset,scissor,unsigned(left),unsigned(top),unsigned(right),unsigned(bottom),vertices[2].color);
    if (!bounds) return {};
    const auto area=(std::int64_t(vertices[1].x)-vertices[0].x)*(std::int64_t(vertices[2].y)-vertices[0].y)-
        (std::int64_t(vertices[1].y)-vertices[0].y)*(std::int64_t(vertices[2].x)-vertices[0].x);
    if (!area) return {};
    if (area<0) std::swap(vertices[1],vertices[2]);
    for (auto& vertex:vertices) {
        vertex.x-=int(offset&65535); vertex.y-=int((offset>>32)&65535);
    }
    bounds->flags|=triangle_flag|(shaded ? shaded_triangle_flag : 0);
    return SpriteCommand{*bounds,{},vertices};
}
}
