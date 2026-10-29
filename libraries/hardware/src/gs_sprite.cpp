#include <nt/gs.hpp>
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
    const unsigned format = (frame>>24)&63;
    if (format > 1) throw runtime::MemoryFault("GS: sprite framebuffer format is not implemented");
    const unsigned pitch = (frame>>16)&63;
    if (!pitch) throw runtime::MemoryFault("GS: zero sprite framebuffer width");
    // GS coordinates are unsigned 12.4, offsets unsigned 16-bit. Coverage is
    // ceil(min) through ceil(max)-1, clipped by inclusive SCISSOR bounds.
    const auto ceil_pixel = [](int value) { return value>=0 ? (value+15)/16 : value/16; };
    const int ox=offset&65535, oy=(offset>>32)&65535;
    const int left=std::max(ceil_pixel(std::min(first.x,last.x)-ox),int(scissor&2047));
    const int right=std::min(ceil_pixel(std::max(first.x,last.x)-ox),int((scissor>>16)&2047)+1);
    const int top=std::max(ceil_pixel(std::min(first.y,last.y)-oy),int((scissor>>32)&2047));
    const int bottom=std::min(ceil_pixel(std::max(first.y,last.y)-oy),int((scissor>>48)&2047)+1);
    if (left>=right || top>=bottom) return;
    auto color=static_cast<std::uint32_t>(last.rgbaq);
    auto mask=static_cast<std::uint32_t>(frame>>32);
    if (format==1) mask |= 0xff000000U;
    if (mask==0xffffffffU) return;
    const unsigned base=(frame&511)*32; // FRAME uses 8192-byte units; pixel_word uses 256.
    graphics::Sprite draw{static_cast<unsigned>(left),static_cast<unsigned>(top),
        static_cast<unsigned>(right),static_cast<unsigned>(bottom),base,pitch,color,mask};
    if (drawing_[0x4a+context]&1) draw.flags |= 32;
    if (attributes&(1U<<4)) {
        const auto texture=drawing_[6+context], filter=drawing_[0x14+context], clamp=drawing_[8+context];
        const unsigned psm=(texture>>20)&63, function=(texture>>35)&3;
        if (!(attributes&(1U<<8))) throw runtime::MemoryFault("GS: perspective sprite textures are not implemented");
        if ((psm>1 && psm!=20) || function>1)
            throw runtime::MemoryFault("GS: texture format/function unsupported: PSM="+std::to_string(psm)+" TFX="+std::to_string(function));
        const unsigned magnify=(filter>>5)&1, minify=(filter>>6)&7;
        if (((filter>>2)&7) || minify>1 || minify!=magnify)
            throw runtime::MemoryFault("GS: mipmapped or mixed sprite filtering unsupported: TEX1="+std::to_string(filter));
        if (magnify) draw.flags |= 128;
        const unsigned tw=(texture>>26)&15, th=(texture>>30)&15;
        if (tw>10 || th>10) throw runtime::MemoryFault("GS: unsupported texture extent");
        draw.texture_base=texture&16383; draw.texture_pitch=(texture>>14)&63;
        draw.texture_width=1U<<tw; draw.texture_height=1U<<th;
        draw.flags |= 1 | (psm==1 ? 2U:0U) | (((texture>>34)&1) ? 4U:0U) | (function<<3);
        if (psm==20) {
            const unsigned cpsm=(texture>>51)&15;
            if (cpsm>1) throw runtime::MemoryFault("GS: 16-bit CLUT sampling is not implemented");
            draw.flags |= 64 | (cpsm==1 ? 2U:0U);
        }
        draw.clamp_u=clamp&3; draw.clamp_v=(clamp>>2)&3;
        draw.min_u=(clamp>>4)&1023; draw.max_u=(clamp>>14)&1023;
        draw.min_v=(clamp>>24)&1023; draw.max_v=(clamp>>34)&1023;
        if ((draw.clamp_u==2 && draw.min_u>draw.max_u) || (draw.clamp_v==2 && draw.min_v>draw.max_v))
            throw runtime::MemoryFault("GS: reversed region-clamp bounds");
        draw.texa=static_cast<std::uint32_t>(drawing_[0x3b]);
        draw.x0=int(first.x)-ox; draw.y0=int(first.y)-oy;
        draw.dx=int(last.x)-first.x; draw.dy=int(last.y)-first.y;
        draw.u0=first.uv&16383; draw.v0=(first.uv>>16)&16383;
        draw.du=int(last.uv&16383)-draw.u0; draw.dv=int((last.uv>>16)&16383)-draw.v0;
    }
    writable_local();
    if (!renderer_) renderer_=std::make_shared<graphics::VulkanRenderer>();
    const auto bank=(drawing_[6+context]>>56)&15;
    renderer_->sprite(*local_,draw,std::span(palette_).subspan(bank*16,16));
}
}
