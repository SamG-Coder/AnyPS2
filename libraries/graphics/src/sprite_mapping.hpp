#pragma once
#include <nt/vulkan_renderer.hpp>
#include <algorithm>
#include <array>

namespace nt::graphics {
// Prove that a sprite's complete sampled footprint, including linear-filter
// neighbors and wrapping, fits an existing native texture surface. The logical
// texture dimensions remain unchanged in the shader.
inline bool texture_fits_surface(const Sprite& draw,unsigned width,unsigned height) {
    if (!(draw.flags&1) || (draw.flags&triangle_flag) || !draw.dx || !draw.dy ||
        draw.left>=draw.right || draw.top>=draw.bottom) return false;
    const auto floor_div=[](std::int64_t n,std::int64_t d) {
        const auto q=n/d,r=n%d;
        return q-(r && ((n<0)!=(d<0)) ? 1 : 0);
    };
    const bool linear=(draw.flags&128)!=0;
    const auto bound=[&](unsigned first,unsigned last,int start,int delta,int uv,int step,
                         unsigned size,unsigned mode,unsigned low,unsigned high) {
        const auto texel=[&](unsigned pixel) {
            const auto fixed=uv+floor_div((std::int64_t(pixel)*16-start)*step,delta)-(linear ? 8 : 0);
            return floor_div(fixed,16);
        };
        auto a=texel(first),b=texel(last-1);
        if (a>b) std::swap(a,b);
        b+=linear ? 1 : 0;
        if (mode==1) return unsigned(std::clamp<std::int64_t>(b,0,size-1));
        if (mode==2) return unsigned(std::clamp<std::int64_t>(b,low,high));
        if (mode==3) return low|high;
        if (floor_div(a,size)!=floor_div(b,size)) return size-1;
        return unsigned(b)&(size-1);
    };
    return bound(draw.left,draw.right,draw.x0,draw.dx,draw.u0,draw.du,draw.texture_width,draw.clamp_u,draw.min_u,draw.max_u)<width &&
        bound(draw.top,draw.bottom,draw.y0,draw.dy,draw.v0,draw.dv,draw.texture_height,draw.clamp_v,draw.min_v,draw.max_v)<height;
}
// PSMCT32 pages are 64x32. Advancing one framebuffer pitch in x
// therefore advances one page row in y, without changing the intra-page
// address. A rectangle no wider than the pitch has disjoint mapped pieces.
inline bool can_map_wrapped_sprite(const Sprite& draw) {
    const auto width=draw.pitch*64;
    if (!width || draw.right<=width || draw.right<=draw.left || draw.bottom<=draw.top ||
        draw.right-draw.left>width || (draw.flags&(1|triangle_flag|line_flag|depth_test_flag))) return false;
    const auto height=draw.bottom+(draw.right-1)/width*32;
    return height<=2048 && ((height+31)/32)*draw.pitch<=512;
}

struct MappedSprites {
    std::array<SpriteCommand,2> commands{};
    unsigned count=0;
};
// Caller validates can_map_wrapped_sprite before emitting any commands.
inline MappedSprites map_wrapped_sprite(const SpriteCommand& command) {
    MappedSprites result;
    const auto& draw=command.draw;
    const auto width=draw.pitch*64;
    for (auto left=draw.left;left<draw.right;) {
        const auto row=left/width;
        const auto right=std::min(draw.right,(row+1)*width);
        auto& part=result.commands[result.count++];
        part=command;
        part.draw.left=left-row*width; part.draw.right=right-row*width;
        part.draw.top+=row*32; part.draw.bottom+=row*32;
        left=right;
    }
    return result;
}
}
