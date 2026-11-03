#pragma once
#include <nt/vulkan_renderer.hpp>
#include <algorithm>
#include <array>

namespace nt::graphics {
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
