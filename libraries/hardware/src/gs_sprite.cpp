#include <nt/gs.hpp>
#include <nt/sprite_plan.hpp>
#include <nt/sprite_texture.hpp>
#include <nt/palette_plan.hpp>
#include <algorithm>

namespace nt::hardware {
void Gs::draw_sprite(const Vertex& first, const Vertex& last) {
    const auto attributes = (drawing_[0x1a]&1) ? drawing_[0] : drawing_[0x1b];
    const unsigned context = (attributes>>9)&1;
    const auto frame = drawing_[0x4c+context], depth = drawing_[0x4e+context];
    const auto test = drawing_[0x47+context], offset = drawing_[0x18+context];
    const auto scissor = drawing_[0x40+context];
    if (attributes & ((1U<<5)|(1U<<6)|(1U<<7)))
        throw runtime::MemoryFault("GS: sprite features unsupported: attributes=" + std::to_string(attributes));
    if ((test & 1) || (test & (1U<<14)) ||
        ((test & (1U<<16)) && (!(depth & (1ULL<<32)) || ((test>>17)&3) != 1)))
        throw runtime::MemoryFault("GS: sprite tests/depth writes unsupported: TEST=" +
                                   std::to_string(test) + " ZBUF=" + std::to_string(depth));
    std::optional<graphics::Sprite> planned;
    try { planned=graphics::plan_sprite(frame,offset,scissor,first.x,first.y,last.x,last.y,static_cast<std::uint32_t>(last.rgbaq)); }
    catch (const std::invalid_argument& error) { throw runtime::MemoryFault(error.what()); }
    if (!planned) return;
    auto draw=*planned;

    if (drawing_[0x4a+context]&1) draw.flags |= 32;
    if (attributes&(1U<<4)) {
        try {
            graphics::plan_sprite_texture(draw,static_cast<unsigned>(attributes),drawing_[6+context],drawing_[0x14+context],
                drawing_[8+context],static_cast<std::uint32_t>(drawing_[0x3b]),offset,first.x,first.y,last.x,last.y,
                static_cast<std::uint32_t>(first.uv),static_cast<std::uint32_t>(last.uv));
        } catch (const std::invalid_argument& error) { throw runtime::MemoryFault(error.what()); }
    }
    writable_local();
    if (!renderer_) renderer_=std::make_shared<graphics::VulkanRenderer>();
    const bool narrow=graphics::palette_is16(drawing_[6+context]);
    const auto bank=(drawing_[6+context]>>56)&(narrow ? 31 : 15);
    std::array<std::uint32_t,256> colors{};
    const auto selected=std::span(colors).first((draw.flags&graphics::indexed8_flag) ? 256 : 16);
    graphics::snapshot_palette(selected,palette_,unsigned(bank),narrow,drawing_[0x3b]);
    renderer_->sprite(*local_,draw,selected);
}
}
