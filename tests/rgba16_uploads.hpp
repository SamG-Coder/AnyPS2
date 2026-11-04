#pragma once
#include <nt/native_drawing.hpp>
#include <stdexcept>

inline void rgba16_uploads(nt::graphics::VulkanRenderer& renderer,bool resident) {
    using namespace nt::graphics;
    const auto require=[](bool ok,const char* message) { if (!ok) throw std::runtime_error(message); };
    struct Point { unsigned x,y,normal,alternate; };
    for (const auto p:std::array{Point{0,0,0,0},Point{1,0,2,2},Point{8,0,1,1},
        Point{0,1,4,4},Point{0,2,32,32},Point{0,4,64,64},Point{16,0,256,256},
        Point{0,8,128,128},Point{32,0,1024,2048},Point{0,16,512,1024},
        Point{0,32,2048,512},Point{64,0,4096,4096},Point{0,64,8192,8192}}) {
        require(rgba_halfword(0,2,p.x,p.y)==p.normal,"PSMCT16 layout boundary differs");
        require(rgba_halfword(0,2,p.x,p.y,true)==p.alternate,"PSMCT16S layout boundary differs");
    }
    for (bool alternate:{false,true}) {
        std::array<bool,4096> occupied{};
        for (unsigned y=0;y<64;++y) for (unsigned x=0;x<64;++x) {
            const auto address=rgba_halfword(0,1,x,y,alternate);
            require(address<occupied.size() && !occupied[address],"16-bit image page overlaps");
            occupied[address]=true;
        }
        require(rgba_halfword(16383,1,16,0,alternate)==128 &&
            rgba_halfword(0,1,2048,2048,alternate)==0,"16-bit upload address wrap differs");
        NativeDrawing drawing(resident);
        const auto setup=[&](unsigned format,unsigned width,unsigned height) {
            Packet result{{0x8004,0x10000000,14,0}};
            for (auto [reg,value]:std::array<std::pair<unsigned,std::uint64_t>,4>{{
                {0x50,(1ULL<<48)|(std::uint64_t(format)<<56)}, {0x51,0},
                {0x52,width|(std::uint64_t(height)<<32)}, {0x53,0}}})
                result.push_back({std::uint32_t(value),std::uint32_t(value>>32),reg,0});
            return result;
        };
        // Seed both halves through a different format, then overwrite only one
        // half at selected addresses using split IMAGE packets and odd rows.
        Packet seed{{0x8010,0x08000000,0,0}};
        seed.resize(17,Quadword{0xa5a55a5a,0xa5a55a5a,0xa5a55a5a,0xa5a55a5a});
        drawing.submit(renderer,std::array{setup(0,16,4),seed});
        auto expected=drawing.pixels();
        constexpr std::array<unsigned,17> row{0,2,8,10,16,18,24,26,1,3,9,11,17,19,25,27,256};
        Packet payload(5,Quadword{~0U,~0U,~0U,~0U});
        for (unsigned i=0;i<34;++i) {
            const unsigned value=(i*1901+1)&65535,shift=(i%2)*16;
            auto& word=payload[i/8][(i%8)/2];
            word=(word&~(65535U<<shift))|(value<<shift);
            const unsigned half=row[i%17]+(i/17)*4,target_shift=(half%2)*16;
            expected[half/2]=(expected[half/2]&~(65535U<<target_shift))|(value<<target_shift);
        }
        drawing.submit(renderer,std::array{setup(alternate ? 10 : 2,17,2)});
        for (const auto& q:payload) drawing.submit(renderer,std::array{Packet{{0x8001,0x08000000,0,0},q}});
        require(drawing.pixels()==expected,"16-bit GPU upload changed pixels, padding or neighboring halves");
        // Unsupported traversal must reject the entire batch before changing storage.
        auto invalid=setup(2,17,2); invalid[2][1]|=1U<<27;
        bool rejected=false;
        try { drawing.submit(renderer,std::array{setup(0,16,4),seed,invalid}); }
        catch (const std::invalid_argument&) { rejected=true; }
        require(rejected && drawing.pixels()==expected,"Rejected 16-bit upload partially committed");
    }
}
