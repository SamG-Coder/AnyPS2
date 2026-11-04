#pragma once
#include <nt/vulkan_renderer.hpp>
#include <stdexcept>
#include <string>

namespace nt::graphics {
inline void plan_texture(Sprite& draw,std::uint64_t texture,
    std::uint64_t filter,std::uint64_t clamp,std::uint32_t texa) {
    const unsigned format=(texture>>20)&63,function=(texture>>35)&3;
    if (format>1 && format!=19 && format!=20)
        throw std::invalid_argument("Unsupported texture format PSM="+std::to_string(format)+" TFX="+std::to_string(function));
    const unsigned magnify=(filter>>5)&1,minify=(filter>>6)&7;
    if (((filter>>2)&7) || minify>1 || minify!=magnify) throw std::invalid_argument("Unsupported mipmap or mixed sprite filtering");
    if (magnify) draw.flags|=128;
    const unsigned tw=(texture>>26)&15,th=(texture>>30)&15;
    if (tw>10 || th>10) throw std::invalid_argument("Unsupported texture extent");
    draw.texture_base=texture&16383; draw.texture_pitch=(texture>>14)&63;
    draw.texture_width=1U<<tw; draw.texture_height=1U<<th;
    draw.flags|=1|(format==1 ? 2U : 0U)|(((texture>>34)&1) ? 4U : 0U)|(function<<3);
    if (format==19 || format==20) {
        const unsigned palette_format=(texture>>51)&15;
        if (palette_format>1 && palette_format!=2 && palette_format!=10)
            throw std::invalid_argument("Unsupported palette sampling format");
        if ((palette_format==2 || palette_format==10) && format==19 && ((texture>>56)&31)>16)
            throw std::invalid_argument("16-bit palette sampling across palette capacity is unsupported");
        draw.flags|=64|(palette_format==1 ? 2U : 0U);
        if (format==19) draw.flags|=indexed8_flag;
    }
    draw.clamp_u=clamp&3; draw.clamp_v=(clamp>>2)&3;
    draw.min_u=(clamp>>4)&1023; draw.max_u=(clamp>>14)&1023;
    draw.min_v=(clamp>>24)&1023; draw.max_v=(clamp>>34)&1023;
    if ((draw.clamp_u==2 && draw.min_u>draw.max_u) || (draw.clamp_v==2 && draw.min_v>draw.max_v))
        throw std::invalid_argument("Reversed region-clamp bounds");
    draw.texa=texa;
}
inline void plan_sprite_texture(Sprite& draw,unsigned attributes,std::uint64_t texture,
    std::uint64_t filter,std::uint64_t clamp,std::uint32_t texa,std::uint64_t offset,
    unsigned first_x,unsigned first_y,unsigned last_x,unsigned last_y,std::uint32_t first_uv,std::uint32_t last_uv) {
    if (!(attributes&(1U<<8))) throw std::invalid_argument("Perspective sprite textures are unsupported");
    plan_texture(draw,texture,filter,clamp,texa);
    draw.x0=int(first_x)-int(offset&65535); draw.y0=int(first_y)-int((offset>>32)&65535);
    draw.dx=int(last_x)-int(first_x); draw.dy=int(last_y)-int(first_y);
    draw.u0=first_uv&16383; draw.v0=(first_uv>>16)&16383;
    draw.du=int(last_uv&16383)-draw.u0; draw.dv=int((last_uv>>16)&16383)-draw.v0;
}
}
