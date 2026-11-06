#pragma once
#include <cstdio>
#include <nt/vulkan_renderer.hpp>
#include <array>
#include <stdexcept>
#include <string>

namespace nt::graphics {
// TEX1 selects the magnification filter for LOD <= 0 and MMIN otherwise.
// MMIN 2/4 select the nearest level and 3/5 blend adjacent levels; MMIN 4/5
// filter bilinearly within a level. Filtering that cannot depend on LOD is
// folded into Sprite flags so ordinary draws keep the single-level path.
inline TextureLevels plan_texture_levels(std::uint64_t filter,const std::array<std::uint64_t,2>& mipmap_bases,bool& linear) {
    const unsigned magnify=(filter>>5)&1,minify=(filter>>6)&7,max_level=(filter>>2)&7;
    if (minify>5) throw std::invalid_argument("Reserved texture minification filter MMIN="+std::to_string(minify));
    if (max_level>6) throw std::invalid_argument("Reserved texture mipmap level count MXL="+std::to_string(max_level));
    const unsigned minify_linear=(minify==1 || minify>=4) ? 1U : 0U;
    const unsigned mip_mode=minify<2 ? 0U : (minify&1) ? 2U : 1U;
    const unsigned levels=mip_mode ? max_level : 0U;
    const bool constant=(filter&1)!=0;
    const auto k=std::uint32_t((filter>>32)&4095); // Signed 1:7:4 fixed point.
    TextureLevels plan{};
    linear=magnify!=0;
    if (constant) {
        const int lod=int(k^2048U)-2048;
        // A constant LOD at or below zero always magnifies level 0.
        if (lod<=0) return plan;
        if (!levels) { linear=minify_linear!=0; return plan; }
    } else if (!levels && minify_linear==magnify) return plan;
    if (levels && ((filter>>9)&1))
        throw std::invalid_argument("Automatic mipmap base addresses (TEX1.MTBA) are unsupported");
    plan.control=texture_lod_enabled|(levels<<1)|(minify_linear<<4)|(mip_mode<<5)|
        (constant ? texture_lod_constant : 0U)|(std::uint32_t((filter>>19)&3)<<8)|(k<<16);
    for (unsigned level=0;level<levels;++level) {
        const auto bases=mipmap_bases[level/3]>>((level%3)*20);
        plan.base[level]=std::uint32_t(bases&16383); plan.pitch[level]=std::uint32_t((bases>>14)&63);
    }
    return plan;
}
inline TextureLevels plan_texture(Sprite& draw,std::uint64_t texture,
    std::uint64_t filter,std::uint64_t clamp,std::uint64_t texa,const std::array<std::uint64_t,2>& mipmap_bases) {
    // A format the GS does not define samples as PSMCT32. Gran Turismo 4 requires this: see
    // "Undefined texture formats" in docs/render-modernization.md.
    const unsigned written_format=(texture>>20)&63,function=(texture>>35)&3;
    const bool defined=written_format<=2 || written_format==10 || written_format==19 || written_format==20 ||
        written_format==0x1b || written_format==0x24 || written_format==0x2c ||
        written_format==0x30 || written_format==0x31 || written_format==0x32 || written_format==0x3a;
    const unsigned format=defined ? written_format : 0;
    const bool depth_layout=format==0x30 || format==0x31;
    // PSMT8H (0x1b), PSMT4HL (0x24) and PSMT4HH (0x2c) index the palette from
    // the top bits of 32-bit words.
    const bool high_index=format==0x1b || format==0x24 || format==0x2c;
    const bool direct_16=format==2 || format==10;
    if (format>1 && format!=19 && format!=20 && !depth_layout && !high_index && !direct_16)
        {
        char value[32];
        std::snprintf(value,sizeof value,"%016llx",static_cast<unsigned long long>(texture));
        throw std::invalid_argument("Unsupported texture format PSM="+std::to_string(format)+" TFX="+std::to_string(function)+" TEX0=0x"+value);
    }
    bool linear=false;
    auto levels=plan_texture_levels(filter,mipmap_bases,linear);
    if (linear) draw.flags|=128;
    const unsigned tw=(texture>>26)&15,th=(texture>>30)&15;
    if (tw>10 || th>10) throw std::invalid_argument("Unsupported texture extent");
    draw.texture_base=texture&16383; draw.texture_pitch=(texture>>14)&63;
    draw.texture_width=1U<<tw; draw.texture_height=1U<<th;
    draw.flags|=1|(format==1 || format==0x31 ? 2U : 0U)|(((texture>>34)&1) ? 4U : 0U)|(function<<3)|(depth_layout ? texture_z_flag : 0U)|
        (direct_16 ? texture_16_flag : 0U)|(format==10 ? texture_16s_flag : 0U);
    if (format==19 || format==20 || high_index) {
        const unsigned palette_format=(texture>>51)&15;
        if (palette_format>1 && palette_format!=2 && palette_format!=10)
            throw std::invalid_argument("Unsupported palette sampling format");
        if ((palette_format==2 || palette_format==10) && (format==19 || format==0x1b) && ((texture>>56)&31)>16)
            throw std::invalid_argument("16-bit palette sampling across palette capacity is unsupported");
        draw.flags|=64|(palette_format==1 ? 2U : 0U);
        if (format==19 || format==0x1b) draw.flags|=indexed8_flag;
        if (high_index) draw.flags|=texture_high_flag|(format==0x2c ? texture_high_nibble_flag : 0U);
    }
    draw.clamp_u=clamp&3; draw.clamp_v=(clamp>>2)&3;
    draw.min_u=(clamp>>4)&1023; draw.max_u=(clamp>>14)&1023;
    draw.min_v=(clamp>>24)&1023; draw.max_v=(clamp>>34)&1023;
    if ((draw.clamp_u==2 && draw.min_u>draw.max_u) || (draw.clamp_v==2 && draw.min_v>draw.max_v))
        throw std::invalid_argument("Reversed region-clamp bounds");
    // TA0 and AEM keep their register positions; TA1 follows them.
    draw.texa=std::uint32_t(texa&0xffffU)|(std::uint32_t((texa>>32)&255U)<<16);
    return levels;
}
inline TextureLevels plan_sprite_texture(Sprite& draw,unsigned attributes,std::uint64_t texture,
    std::uint64_t filter,std::uint64_t clamp,std::uint64_t texa,const std::array<std::uint64_t,2>& mipmap_bases,
    std::uint64_t offset,unsigned first_x,unsigned first_y,unsigned last_x,unsigned last_y,
    std::uint32_t first_uv,std::uint32_t last_uv,float q) {
    if (!(attributes&(1U<<8))) throw std::invalid_argument("Perspective sprite textures are unsupported");
    auto levels=plan_texture(draw,texture,filter,clamp,texa,mipmap_bases);
    levels.q=q;
    draw.x0=int(first_x)-int(offset&65535); draw.y0=int(first_y)-int((offset>>32)&65535);
    draw.dx=int(last_x)-int(first_x); draw.dy=int(last_y)-int(first_y);
    draw.u0=first_uv&16383; draw.v0=(first_uv>>16)&16383;
    draw.du=int(last_uv&16383)-draw.u0; draw.dv=int((last_uv>>16)&16383)-draw.v0;
    return levels;
}
}
