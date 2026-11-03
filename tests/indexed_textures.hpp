#pragma once
#include <nt/native_drawing.hpp>
#include <stdexcept>

// Synthetic uploads and independent expected colors exercise the complete
// upload -> CLUT -> cache -> native image sampling path without a window.
inline void indexed8_textures(nt::graphics::VulkanRenderer& renderer,bool resident) {
    using namespace nt::graphics;
    const auto require=[](bool ok,const char* message) { if (!ok) throw std::runtime_error(message); };
    struct Point { unsigned x,y,address; };
    for (const auto p:std::array{Point{0,0,0},Point{1,0,4},Point{0,1,8},Point{0,2,33},
        Point{0,4,96},Point{0,8,128},Point{16,0,256},Point{0,16,512},
        Point{128,0,8192},Point{0,64,16384}})
        require(indexed_byte(0,4,p.x,p.y)==p.address,"PSMT8 byte layout differs at a column/block/page boundary");
    std::array<bool,8192> occupied{};
    for (unsigned y=0;y<64;++y) for (unsigned x=0;x<128;++x) {
        const auto address=indexed_byte(0,2,x,y);
        require(address<occupied.size() && !occupied[address],"PSMT8 page contains an overlapping byte");
        occupied[address]=true;
    }
    const auto ad=[](std::initializer_list<std::pair<unsigned,std::uint64_t>> values) {
        Packet packet{{0x8000U|unsigned(values.size()),0x10000000,14,0}};
        for (auto [reg,value]:values) packet.push_back({std::uint32_t(value),std::uint32_t(value>>32),reg,0});
        return packet;
    };
    const auto upload=[&](unsigned base,unsigned pitch,unsigned format,unsigned width,unsigned height) {
        return ad({{0x50,(std::uint64_t(base)<<32)|(std::uint64_t(pitch)<<48)|(std::uint64_t(format)<<56)},
            {0x51,0},{0x52,width|(std::uint64_t(height)<<32)},{0x53,0}});
    };
    NativeDrawing drawing(resident);
    Packet indices{{0x8000U|1024,0x08000000,0,0}};
    for (unsigned q=0;q<1024;++q) {
        Quadword words{};
        for (unsigned b=0;b<16;++b) words[b/4]|=((q*16+b)&255)<<(8*(b%4));
        indices.push_back(words);
    }
    Packet colors{{0x8040,0x08000000,0,0}};
    for (unsigned q=0;q<64;++q) colors.push_back({0x80000000U+(q*4)*0x10101U,
        0x80000000U+(q*4+1)*0x10101U,0x80000000U+(q*4+2)*0x10101U,0x80000000U+(q*4+3)*0x10101U});
    const auto texture=[&](unsigned load,bool storage2,unsigned bank=0) {
        return (2ULL<<14)|(19ULL<<20)|(7ULL<<26)|(7ULL<<30)|(1ULL<<34)|(1ULL<<35)|
            (128ULL<<37)|(std::uint64_t(storage2)<<55)|(std::uint64_t(bank)<<56)|(std::uint64_t(load)<<61);
    };
    const auto draw=[&](unsigned load,bool storage2,unsigned bank=0) {
        return ad({{0x1a,1},{0,6|16|256},{0x4c,128|(2ULL<<16)},
            {0x40,(127ULL<<16)|(127ULL<<48)},{1,0x80808080},{6,texture(load,storage2,bank)},
            {3,0},{5,0},{3,2048|(2048ULL<<16)},{5,2048|(2048ULL<<16)}});
    };
    const auto verify=[&](bool storage2,unsigned change=0,unsigned bank=0) {
        const auto& pixels=drawing.pixels();
        for (unsigned y=0;y<128;++y) for (unsigned x=0;x<128;++x) {
            const unsigned index=(x+y*128)&255;
            const unsigned selected=std::min(bank+index/16,15U)*16+(index&15);
            const unsigned color=storage2 ? selected : (selected&~24U)|((selected&8)<<1)|((selected&16)>>1);
            require(pixels[rgba_word(4096,2,x,y)]==((0x80000000U+color*0x10101U)^change),
                "PSMT8 upload/palette/sampling produced the wrong color");
        }
    };
    drawing.submit(renderer,std::array{upload(0,2,19,128,128),indices,upload(128,1,0,16,16),colors,draw(2,false)});
    verify(false);
    for (std::size_t q=1;q<colors.size();++q) for (auto& word:colors[q]) word^=0x00408020;
    drawing.submit(renderer,std::array{upload(128,1,0,16,16),colors,draw(4,false)});
    verify(false); // Conditional CLD keeps the previous palette snapshot.
    drawing.submit(renderer,std::array{draw(1,false)}); verify(false,0x00408020);
    drawing.submit(renderer,std::array{draw(1,false,3)}); verify(false,0x00408020,3);
    drawing.submit(renderer,std::array{upload(128,4,0,256,1),colors,ad({{0x1c,4}}),draw(1,true)});
    verify(true,0x00408020);
    const auto before=drawing.pixels(); bool rejected=false;
    try { drawing.submit(renderer,std::array{draw(1,true,1)}); } catch (const std::invalid_argument&) { rejected=true; }
    require(rejected && drawing.pixels()==before,"Unsupported CSM2 bank partially published a draw");
}
