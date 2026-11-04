#pragma once
#include <nt/native_drawing.hpp>
#include <stdexcept>

inline void palette16_textures(nt::graphics::VulkanRenderer& renderer,bool resident) {
    using namespace nt::graphics;
    const auto ad=[](std::initializer_list<std::pair<unsigned,std::uint64_t>> values) {
        Packet packet{{0x8000U|unsigned(values.size()),0x10000000,14,0}};
        for (auto [reg,value]:values) packet.push_back({std::uint32_t(value),std::uint32_t(value>>32),reg,0});
        return packet;
    };
    const auto upload=[&](unsigned base,unsigned format,unsigned width,unsigned height,std::uint64_t position=0) {
        return ad({{0x50,(std::uint64_t(base)<<32)|(4ULL<<48)|(std::uint64_t(format)<<56)},
            {0x51,position},{0x52,width|(std::uint64_t(height)<<32)},{0x53,0}});
    };
    const auto color=[](unsigned i) {
        if (i<2) return i*0x8000U; // AEM must distinguish transparent and opaque black.
        return ((i*3)&31)|(((i*5)&31)<<5)|(((i*7)&31)<<10)|((i&1)<<15);
    };
    {
        std::array<std::uint32_t,256> palette{};
        palette.fill(0x12345678);
        std::array<std::uint32_t,16> output{},words{};
        snapshot_palette(output,palette,31,true,0);
        if (output[0]!=0x002088a0) throw std::runtime_error("Upper 16-bit palette plane lost its 32-bit alias");
        std::array<unsigned,2> bases{};
        const auto load=plan_palette((20ULL<<20)|(2ULL<<51)|(31ULL<<56)|(1ULL<<61),0,bases);
        words.fill(0xffffffff);
        apply_palette(palette,words,*load);
        snapshot_palette(output,palette,15);
        if (output[0]!=0xffff5678 || palette[0]!=0x12345678)
            throw std::runtime_error("16-bit palette load overwrote the other color plane or bank");
    }
    for (unsigned format:{2U,10U}) for (bool storage2:{false,true}) for (bool eight:{false,true}) {
        const unsigned count=eight ? 256 : 16,bank=storage2 ? 0 : eight ? 16 : 31;
        const unsigned width=storage2 ? count : eight ? 16 : 8,height=count/width;
        const std::uint64_t position=storage2 ? (16ULL<<32)|(32ULL<<48) : 0;
        NativeDrawing drawing(resident);
        Packet colors(1+(count+7)/8,Quadword{});
        colors[0]={0x8000U|unsigned(colors.size()-1),0x08000000,0,0};
        for (unsigned i=0;i<count;++i) {
            // CSM1 interchanges the two middle index bits in a 16x16 palette;
            // the 16-entry case is an 8x2 rectangle.
            const unsigned at=(!storage2 && eight) ? (i&~24U)|((i&8)<<1)|((i&16)>>1) : i;
            colors[1+at/8][(at%8)/2]|=color(i)<<((at%2)*16);
        }
        Packet indices(1+(count*(eight ? 8 : 4)+127)/128,Quadword{});
        indices[0]={0x8000U|unsigned(indices.size()-1),0x08000000,0,0};
        for (unsigned i=0;i<count;++i) {
            const unsigned bit=i*(eight ? 8 : 4);
            indices[1+bit/128][(bit%128)/32]|=i<<(bit%32);
        }
        const auto texture=[&](unsigned load) {
            return (4ULL<<14)|(std::uint64_t(eight ? 19 : 20)<<20)|
                (std::uint64_t(eight ? 8 : 4)<<26)|(1ULL<<34)|(1ULL<<35)|(128ULL<<37)|
                (std::uint64_t(format)<<51)|(std::uint64_t(storage2)<<55)|
                (std::uint64_t(bank)<<56)|(std::uint64_t(load)<<61);
        };
        const auto draw=[&](unsigned load,std::uint64_t texa) {
            return ad({{0x1a,1},{0,6|16|256},{0x4c,128|(4ULL<<16)},
                {0x40,std::uint64_t(count-1)<<16},{0x46,1},{1,0x80808080},
                {0x3b,texa},{6,texture(load)},{3,0},{5,0},
                {3,count*16},{5,count*16|(16ULL<<16)}});
        };
        const auto verify=[&](std::uint64_t texa) {
            const auto& pixels=drawing.pixels();
            for (unsigned i=0;i<count;++i) {
                const unsigned c=color(i);
                const unsigned alpha=((texa&0x8000) && c==0) ? 0 : ((texa>>((c&0x8000) ? 32 : 0))&255);
                const unsigned expected=(((i*3)&31)*8)|((((i*5)&31)*8)<<8)|((((i*7)&31)*8)<<16);
                const unsigned rgb=i<2 ? 0 : expected;
                if (pixels[rgba_word(4096,4,i,0)]!=(rgb|(alpha<<24)))
                    throw std::runtime_error("16-bit palette upload/bank/TEXA sampling differs: format="+
                        std::to_string(format)+" csm2="+std::to_string(storage2)+" index="+std::to_string(i));
            }
        };
        constexpr std::uint64_t first_alpha=17|(129ULL<<32),second_alpha=0x8000|43|(201ULL<<32);
        drawing.submit(renderer,std::array{upload(0,eight ? 19 : 20,count,1),indices,
            upload(128,format,width,height,position),colors,ad({{0x1c,4|(1U<<6)|(32U<<12)}}),draw(2,first_alpha)});
        verify(first_alpha);
        // Source changes do not replace a cached palette under CLD=0 or a
        // matching conditional load. TEXA changes still affect the next draw.
        for (std::size_t i=1;i<colors.size();++i) colors[i]={~0U,~0U,~0U,~0U};
        drawing.submit(renderer,std::array{upload(128,format,width,height,position),colors,draw(0,second_alpha)});
        verify(second_alpha);
        drawing.submit(renderer,std::array{draw(4,first_alpha)});
        verify(first_alpha);
    }
}
