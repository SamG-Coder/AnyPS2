#include "indexed_textures.hpp"
#include <nt/vulkan_renderer.hpp>
#include <nt/native_drawing_worker.hpp>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
using namespace nt::graphics;
int main(int argc,char** argv) {
    try {
        VulkanRenderer native(nullptr,true),reference(nullptr,true,RenderPath::reference_compute);
        if (argc>1 && (std::string_view(argv[1])!="--require-llvm-textures" || !native.statistics().llvm_texture_kernels))
            throw std::runtime_error("Expected the LLVM texture kernel renderer");
        std::vector<std::uint32_t> expected(1024*1024),actual=expected;
        unsigned checks=0;
        const auto compare=[&](std::span<const SpriteCommand> commands) {
            native.upload(actual); reference.upload(expected);
            native.draw(commands); reference.draw(commands);
            native.read(actual); reference.read(expected);
            for (unsigned i=0;i<actual.size();++i) if (actual[i]!=expected[i])
                throw std::runtime_error("Native pixel differs in case "+std::to_string(checks)+" at word "+std::to_string(i)+
                    " actual="+std::to_string(actual[i])+" expected="+std::to_string(expected[i]));
            ++checks;
        };
        SpriteCommand clear{{0,0,32,32,0,1,0x80203040,0}};
        {
            // Fog uses affine vertex factors, a 256 divisor and unchanged alpha.
            // Exercise packed XYZF and the separate FOG latch through real packets.
            for (bool packed:{false,true}) {
                NativeDrawing fogged(true);
                Packet setup{{0x8007,0x10000000,14,0},{1,0,0x1a,0},{3|8|32,0,0,0},
                    {1<<16,0,0x4c,0},{15<<16,15<<16,0x40,0},{1,0,0x46,0},
                    {0x800000ff,0x3f800000,1,0},{0x00ff0000,0,0x3d,0}};
                Packet vertices{{0x8000,0x10000000,packed ? 4U : 14U,0}};
                const std::array<std::array<unsigned,3>,3> inputs{{{0,0,0},{256,0,128},{0,256,255}}};
                for (const auto& v:inputs) {
                    if (packed) vertices.push_back({v[0],v[1],0,v[2]<<4});
                    else {
                        vertices.push_back({0,v[2]<<24,0xa,0});
                        vertices.push_back({v[0]|(v[1]<<16),0,5,0});
                    }
                }
                vertices[0][0]|=unsigned(vertices.size()-1);
                fogged.submit(native,std::array{setup,vertices});
                const auto& pixels=fogged.pixels();
                if (pixels[rgba_word(0,1,4,4)]!=0x809f005fU || pixels[rgba_word(0,1,1,1)]!=0x80e70017U)
                    throw std::runtime_error("Native fog packet, interpolation, color or alpha mismatch");
            }
        }
        // Original packet fixtures: all octants, fractional positions, endpoint
        // exclusion, packet-spanning strips, suppressed kicks and depth/blending.
        // Explicit pixel expectations are independent of the shared GPU helper.
        {
            const auto packet_for=[](unsigned primitive,std::span<const RasterVertex> vertices) {
                Packet p{{0,0x10000000,14,0},{1,0,0x1a,0},{primitive,0,0,0},{1<<16,0,0x4c,0},
                    {15<<16,15<<16,0x40,0},{1,0,0x46,0},{0x44,0,0x42,0}};
                for (const auto& v:vertices) {
                    p.push_back({v.color,0,1,0});
                    p.push_back({unsigned(v.x)|(unsigned(v.y)<<16),v.z,5,0});
                }
                p[0][0]=0x8000U|unsigned(p.size()-1); return p;
            };
            const auto verify=[&](const Packet& p,std::span<const std::pair<unsigned,unsigned>> points,std::uint32_t color=0x80402010) {
                NativeDrawing drawing(true);
                drawing.submit(native,std::array{p});
                std::vector<std::uint32_t> wanted(1024*1024);
                for (auto [x,y]:points) wanted[rgba_word(0,1,x,y)]=color;
                if (drawing.pixels()!=wanted) throw std::runtime_error("Point/line coverage differs from expected pixels");
            };
            for (bool steep:{false,true}) for (bool reverse_x:{false,true}) for (bool reverse_y:{false,true}) {
                const auto transform=[&](int x,int y) {
                    if (reverse_x) x=8-x;
                    if (reverse_y) y=8-y;
                    return steep ? std::pair{y,x} : std::pair{x,y};
                };
                auto [ax,ay]=transform(1,1); auto [bx,by]=transform(5,3);
                const std::array v{RasterVertex{ax*16,ay*16,0x80402010},RasterVertex{bx*16,by*16,0x80402010}};
                std::vector<std::pair<unsigned,unsigned>> points;
                // Ties round toward positive coordinates, also for negative slopes.
                for (int i=0;i<4;++i) {
                    const int x=ax+(bx-ax)*i/4;
                    const int y=ay+(by-ay)*i/4;
                    int px=x,py=y;
                    if (steep) px=(ax*4+(bx-ax)*i+2)/4;
                    else py=(ay*4+(by-ay)*i+2)/4;
                    points.emplace_back(unsigned(px),unsigned(py));
                }
                verify(packet_for(1,v),points);
            }
            const std::array fractional{RasterVertex{24,16,0x80402010},RasterVertex{72,16,0x80402010}};
            const std::array fractional_pixels{std::pair{2U,1U},std::pair{3U,1U},std::pair{4U,1U}};
            verify(packet_for(1,fractional),fractional_pixels);
            const std::array point{RasterVertex{24,39,0x80402010}};
            const std::array point_pixels{std::pair{2U,2U}};
            verify(packet_for(0,point),point_pixels);
            verify(packet_for(0|128,point),point_pixels);
            // A large submission of individually bounded packets is validated
            // atomically, including the final packet after the old 1 MiB cap.
            Packet padding(32769);
            padding.front()={32767,0x10000000,15,0}; padding.back()[0]=0x8000;
            std::array large_batch{padding,padding,packet_for(0,point)};
            NativeDrawing large(true);
            large.submit(native,large_batch);
            const auto large_before=large.pixels();
            if (large_before[rgba_word(0,1,2,2)]!=0x80402010 || large.submitted_draws()!=1)
                throw std::runtime_error("Large native graphics batch lost its final draw");
            large_batch.back().pop_back();
            bool large_rejected=false;
            try { large.submit(native,large_batch); } catch (const std::invalid_argument&) { large_rejected=true; }
            if (!large_rejected || large.pixels()!=large_before || large.submitted_draws()!=1)
                throw std::runtime_error("Malformed large batch partially published graphics");
            const std::array strip{RasterVertex{16,16,0x80402010},RasterVertex{64,16,0x80402010},RasterVertex{64,64,0x80402010}};
            const std::array strip_pixels{std::pair{1U,1U},std::pair{2U,1U},std::pair{3U,1U},
                std::pair{4U,1U},std::pair{4U,2U},std::pair{4U,3U}};
            auto translucent=strip;
            for (auto& v:translucent) v.color=0x40402010;
            verify(packet_for(2|64,translucent),strip_pixels,0x40201008); // Duplicate shared endpoints would blend twice.
            NativeDrawing split(true);
            auto first=packet_for(2,std::span(strip).first(1));
            split.submit(native,std::array{first});
            Packet rest{{0x8004,0x10000000,14,0},{strip[1].color,0,1,0},{64|(16<<16),0,13,0},
                {strip[2].color,0,1,0},{64|(64<<16),0,5,0}};
            split.submit(native,std::array{rest});
            std::vector<std::uint32_t> wanted(1024*1024);
            for (unsigned y=1;y<4;++y) wanted[rgba_word(0,1,4,y)]=0x80402010;
            if (split.pixels()!=wanted) throw std::runtime_error("Line strip lost a suppressed or packet-spanning vertex");
            const auto before=split.pixels();
            auto unsupported=packet_for(1|16,std::span(strip).first(2));
            bool rejected=false;
            try { split.submit(native,std::array{unsupported}); } catch (const std::invalid_argument&) { rejected=true; }
            if (!rejected || split.pixels()!=before) throw std::runtime_error("Unsupported line partially published a batch");

            for (bool vertical:{false,true}) for (bool reverse:{false,true}) for (bool blend:{false,true}) {
                const unsigned color=blend ? 0x40808080U : 0x80808080U;
                std::array aa_vertices{RasterVertex{16,20,color},RasterVertex{80,20,color}};
                if (vertical) for (auto& v:aa_vertices) std::swap(v.x,v.y);
                if (reverse) std::swap(aa_vertices[0],aa_vertices[1]);
                NativeDrawing aa(true);
                aa.submit(native,std::array{packet_for(1|128|(blend ? 64 : 0),aa_vertices)});
                std::vector<std::uint32_t> wanted_aa(1024*1024);
                for (unsigned major=reverse ? 2 : 1;major<(reverse ? 6U : 5U);++major)
                    for (unsigned minor=1;minor<=2;++minor)
                        wanted_aa[rgba_word(0,1,vertical ? minor : major,vertical ? major : minor)]=
                            blend ? 0x40404040U : minor==1 ? 0x60606060U : 0x1f1f1f1fU;
                if (aa.pixels()!=wanted_aa) throw std::runtime_error("Native AA line coverage, endpoint or blend-alpha rule differs");
            }

            auto gradient=*plan_line_or_point(1U<<16,0,(15ULL<<16)|(15ULL<<48),
                {RasterVertex{16,32,0x80000000,0,0,0,0,1,100},RasterVertex{80,32,0x80808080,0,0,0,0,1,104}},false,true);
            gradient.draw.flags|=depth_test_flag|depth_write_flag;
            gradient.draw.depth_base=32; gradient.draw.test=1<<17;
            actual.assign(actual.size(),0); expected=actual;
            compare(std::array{gradient});
            for (unsigned x=1;x<5;++x) {
                if (actual[rgba_word(0,1,x,2)]!=(0x80000000U|(x-1)*0x202020U) ||
                    actual[rgba_word(32,1,x,2)^0x600] != 99+x)
                    throw std::runtime_error("Native line color/depth interpolation differs");
            }
            auto antialiased=*plan_line_or_point(1U<<16,0,(15ULL<<16)|(15ULL<<48),
                {RasterVertex{16,36,0x80808080,0,0,0,0,1,100},RasterVertex{80,36,0x80808080,0,0,0,0,1,104}},false,false,true);
            antialiased.draw.flags|=coverage_flag|256|depth_test_flag|depth_write_flag;
            antialiased.draw.blend=0x44; antialiased.draw.depth_base=32; antialiased.draw.test=1<<17;
            actual.assign(actual.size(),0); expected=actual;
            compare(std::array{antialiased});
            for (unsigned x=1;x<5;++x) for (unsigned y=2;y<=3;++y)
                if (actual[rgba_word(0,1,x,y)]!=(y==2 ? 0x60606060U : 0x1f1f1f1fU) ||
                    actual[rgba_word(32,1,x,y)^0x600])
                    throw std::runtime_error("AA line coverage changed destination depth or lost its neighboring pixel");
            actual.assign(actual.size(),0); expected=actual;
        }
        // Constant-alpha display composition applies to RC1 even when RC2 is
        // disabled. Disabled circuits must not be sampled. RC2 alone bypasses
        // the RC1 blend, regardless of the constant alpha.
        {
            std::vector<std::uint32_t> white(actual.size(),0xffffffffU);
            native.upload(white); reference.upload(white);
            const std::array<std::uint64_t,2> buffers{1U<<9,1U<<9};
            const std::array<std::uint64_t,2> displays{(1ULL<<32)|(1ULL<<44),(1ULL<<32)|(1ULL<<44)};
            struct DisplayCase { unsigned alpha,background,pixel; };
            for (const auto test:{DisplayCase{0,0,0xff000000},DisplayCase{128,0,0xff808080},
                                 DisplayCase{255,0,0xffffffff},DisplayCase{128,0x00302010,0xff989088}})
                for (unsigned background_select:{0U,128U}) {
                    const auto control=1U|32U|background_select|(test.alpha<<8);
                    const auto plan=*plan_display(control,buffers,displays,false,test.background);
                    const auto a=native.scanout_readback(plan),b=reference.scanout_readback(plan);
                    if (a.pixels!=std::vector<std::uint32_t>(4,test.pixel) || a.pixels!=b.pixels)
                        throw std::runtime_error("Single-circuit constant-alpha display composition differs");
                }
            const auto plan=*plan_display(2U|32U,buffers,displays,false,0x00302010);
            const auto a=native.scanout_readback(plan),b=reference.scanout_readback(plan);
            if (a.pixels!=std::vector<std::uint32_t>(4,0xffffffff) || a.pixels!=b.pixels)
                throw std::runtime_error("RC2-only output incorrectly uses the RC1 blend");
        }
        // A read-only display view can extend beyond a smaller drawing target.
        // Exporting newer pixels must invalidate that overlapping cached view.
        {
            native.upload(std::vector<std::uint32_t>(actual.size()));
            SpriteCommand paint{{0,0,64,32,0,1,0x800000ff,0}};
            native.draw(std::array{paint});
            const std::array<std::uint64_t,2> displays{63ULL|(65ULL<<44),63ULL|(65ULL<<44)};
            const auto plan=*plan_display(2,{512,512},displays,true,0,DisplayField::even);
            if (native.scanout_readback(plan).pixels.front()!=0xff0000ffU)
                throw std::runtime_error("Extended display view lost initial drawing");
            paint.draw.color=0x8000ff00;
            native.draw(std::array{paint});
            std::vector<std::uint32_t> exported(actual.size()); native.read(exported);
            if (native.scanout_readback(plan).pixels.front()!=0xff00ff00U)
                throw std::runtime_error("Exported drawing left a stale overlapping display view");
            ++checks;
        }
        // Extended display aliases compose dirty color/depth pages directly.
        // Exercise different pitches, storage wrap and CPU invalidation against
        // the reference path, while requiring no intermediate writer readback.
        for (unsigned base:{0U,16352U,1U}) for (unsigned display_pitch:{1U,2U}) {
            std::vector<std::uint32_t> initial(actual.size(),0x80402010);
            native.upload(initial); reference.upload(initial);
            std::array<SpriteCommand,4> tiles;
            for (unsigned n=0;n<tiles.size();++n) {
                auto& draw=tiles[n].draw;
                draw={(n%2)*32,(n/2)*16,(n%2+1)*32,(n/2+1)*16,base,1,
                    0x80000020U+n*0x00201008U,0};
                draw.flags=depth_test_flag|depth_write_flag;
                draw.depth_base=((base+63)&~31U)&16383; draw.test=1U<<17;
                draw.depth=0x80506070U+n*0x00010305U;
            }
            native.draw(tiles); reference.draw(tiles);
            const unsigned width=display_pitch*64,height=96/display_pitch;
            const auto buffer=std::uint64_t(base/32)|(std::uint64_t(display_pitch)<<9);
            const auto display=(std::uint64_t(width-1)<<32)|(std::uint64_t(height*2-1)<<44);
            const auto plan=*plan_display(2,{buffer,buffer},{display,display},true,0,DisplayField::even);
            const auto before=native.statistics().readback_bytes;
            const auto composed=native.scanout_readback(plan);
            if (composed.pixels!=reference.scanout_readback(plan).pixels)
                throw std::runtime_error("GPU display page composition changed color/depth aliases");
            if (base%32==0 && native.statistics().readback_bytes-before!=composed.pixels.size()*sizeof(std::uint32_t))
                throw std::runtime_error("Display composition exported an intermediate writer");
            // A later draw must replace the cached view, and a partial CPU write
            // must preserve the other bytes before invalidating it.
            tiles[0].draw.color=0x80776655; tiles[0].draw.depth=0x80332211;
            native.draw(std::span(tiles).first(1)); reference.draw(std::span(tiles).first(1));
            if (native.scanout_readback(plan).pixels!=reference.scanout_readback(plan).pixels)
                throw std::runtime_error("GPU display alias retained an older writer");
            const std::array writes{PixelWrite{rgba_word(base,1,3,5),0x0000aa00,0x0000ff00}};
            native.write(writes); reference.write(writes);
            if (native.scanout_readback(plan).pixels!=reference.scanout_readback(plan).pixels)
                throw std::runtime_error("CPU write left a stale GPU display alias");
            native.read(actual); reference.read(expected);
            if (actual!=expected) throw std::runtime_error("Display composition changed eventual CPU backing contents");
            ++checks;
        }
        // Relative display origins are independent of source buffer origins.
        // Verify both displacement directions, unequal extents and odd field
        // offsets against explicit pixels, not just two matching implementations.
        {
            std::vector<std::uint32_t> pixels(actual.size());
            for (unsigned y=0;y<8;++y) for (unsigned x=0;x<8;++x) {
                pixels[rgba_word(0,1,x+2,y+3)]=32+y*16+x;
                pixels[rgba_word(32,1,x+1,y+2)]=(48+y*16+x)<<8;
            }
            native.upload(pixels); reference.upload(pixels);
            const std::array<std::uint64_t,2> buffers{(1ULL<<9)|(2ULL<<32)|(3ULL<<43),
                1|(1ULL<<9)|(1ULL<<32)|(2ULL<<43)};
            for (bool flip:{false,true}) for (bool half:{false,true})
                for (unsigned alpha:{0U,128U,255U}) for (bool background:{false,true}) {
                    const unsigned ax=flip ? 0:2,ay=flip ? 0:1,bx=flip ? 2:0,by=flip ? 1:0;
                    const auto display=[](unsigned x,unsigned y,unsigned w,unsigned h) {
                        return std::uint64_t(100+x*4)|(std::uint64_t(50+y)<<12)|(3ULL<<23)|
                            (std::uint64_t(w*4-1)<<32)|(std::uint64_t(h-1)<<44);
                    };
                    const std::array<std::uint64_t,2> displays{display(ax,ay,3,3),display(bx,by,4,4)};
                    const auto plan=*plan_display(35U|(alpha<<8)|(background ? 128:0),buffers,displays,half,0x600000);
                    const unsigned width=flip ? 6:5,height=flip ? 5:4;
                    if (plan.width!=width || plan.height!=height)
                        throw std::runtime_error("Displaced display union has incorrect dimensions");
                    std::vector<std::uint32_t> wanted(width*height);
                    for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x) {
                        unsigned r=0,g=0,b=96;
                        if (!background && x>=bx && y>=by && x-bx<4 && y-by<4) {
                            g=48+(half ? (y-by)/2 : y-by)*16+x-bx; b=0;
                        }
                        if (x>=ax && y>=ay && x-ax<3 && y-ay<3) {
                            const unsigned red=32+(half ? (y-ay)/2 : y-ay)*16+x-ax;
                            r=(red*alpha+127)/255; g=(g*(255-alpha)+127)/255; b=(b*(255-alpha)+127)/255;
                        }
                        wanted[y*width+x]=0xff000000U|r|(g<<8)|(b<<16);
                    }
                    const auto a=native.scanout_readback(plan),b=reference.scanout_readback(plan);
                    if (a.pixels!=wanted || b.pixels!=wanted)
                        throw std::runtime_error("Displaced display composition lost offsets, field phase or background");
                    ++checks;
                }
            const std::array<std::uint64_t,2> aligned{(3ULL<<23)|(15ULL<<32)|(3ULL<<44),
                (3ULL<<23)|(15ULL<<32)|(3ULL<<44)};
            for (unsigned malformed:{0U,1U,2U}) {
                auto displays=aligned;
                if (malformed==0) displays[1]|=1; // Fractional source-pixel displacement.
                if (malformed==1) displays[1]^=1ULL<<23; // Different magnification.
                if (malformed==2) displays[1]|=2047ULL<<12; // Excessive output union.
                bool rejected=false;
                try { plan_display(35,buffers,displays); } catch (const std::invalid_argument&) { rejected=true; }
                if (!rejected) throw std::runtime_error("Unsupported display mapping was approximated silently");
            }
        }
        // A linearly varying image represented by two interlaced fields must
        // remain aligned. The odd field blends adjacent rows through RC1/RC2;
        // its samples belong between the even field's rows, not on top of them.
        {
            std::vector<std::uint32_t> pixels(actual.size());
            for (unsigned y=0;y<4;++y) pixels[rgba_word(0,1,0,y)]=y*64;
            native.upload(pixels); reference.upload(pixels);
            const std::array<std::uint64_t,2> buffers{(1ULL<<9)|(1ULL<<43),1ULL<<9};
            const std::array<std::uint64_t,2> displays{5ULL<<44,5ULL<<44};
            for (auto field:{DisplayField::even,DisplayField::odd}) {
                const bool odd=field==DisplayField::odd;
                const auto plan=*plan_display(odd ? 35U|(128U<<8) : 2U,buffers,displays,true,0,field);
                const std::array<unsigned,6> wanted=odd ? std::array<unsigned,6>{32,32,64,96,128,160}
                                                       : std::array<unsigned,6>{0,32,64,96,128,128};
                const auto a=native.scanout_readback(plan),b=reference.scanout_readback(plan);
                for (unsigned y=0;y<wanted.size();++y)
                    if (a.pixels[y]!=(0xff000000U|wanted[y]) || b.pixels[y]!=a.pixels[y])
                        throw std::runtime_error("Interlaced fields are not reconstructed at their actual line positions");
                const auto progressive=*plan_display(2,buffers,displays,false,0,field);
                if (progressive.field!=DisplayField::progressive)
                    throw std::runtime_error("Field reconstruction changed progressive output");
                ++checks;
            }
        }
        // Static alternating scan lines must stop flickering after history is
        // established. Changes in either parity must reject stale field data.
        {
            std::vector<std::uint32_t> pixels(actual.size());
            for (unsigned y=0;y<4;++y) pixels[rgba_word(32,1,0,y)]=0x00ffffff;
            native.upload(pixels);
            const std::array<std::uint64_t,2> displays{7ULL<<44,7ULL<<44};
            const auto field_plan=[&](bool odd) {
                return *plan_display(2,{0,(1ULL<<9)|(odd ? 1U:0U)},displays,true,0,
                    odd ? DisplayField::odd : DisplayField::even);
            };
            for (unsigned field=0;field<8;++field) {
                const bool odd=field&1;
                const auto plan=field_plan(odd),copy=plan;
                const auto frame=native.scanout_readback(plan);
                if (native.scanout_readback(copy).pixels!=frame.pixels)
                    throw std::runtime_error("Repeated capture advanced field history");
                for (unsigned y=1;y<8;++y) {
                    const auto expected=0xff000000U|((field>=3 ? y&1 : odd) ? 0xffffffU:0U);
                    if (frame.pixels[y]!=expected)
                        throw std::runtime_error("Stationary interlaced detail flickers between fields");
                }
            }
            std::vector<PixelWrite> writes;
            for (unsigned y=0;y<4;++y) writes.push_back({rgba_word(0,1,0,y),0x0000ff00,0xffffffff});
            native.write(writes);
            for (bool odd:{false,true}) {
                const auto frame=native.scanout_readback(field_plan(odd));
                const auto expected=odd ? 0xffffffffU : 0xff00ff00U;
                if (!std::all_of(frame.pixels.begin(),frame.pixels.end(),[&](auto p){ return p==expected; }))
                    throw std::runtime_error("Changed interlaced detail retained temporal trails");
            }
            // Repeating a parity is a discontinuity, not another adjacent field.
            const auto repeated=field_plan(true);
            native.write(std::array{PixelWrite{rgba_word(32,1,0,0),0x000000ff,0xffffffff}});
            const auto frame=native.scanout_readback(repeated);
            if (frame.pixels[2]!=0xff8080ffU)
                throw std::runtime_error("Repeated field parity reused invalid history");
            ++checks;
        }
        SpriteCommand quad{{2,2,18,18,0,1,0x80406080,0}};
        {
            // Consecutive compatible resources, then palette/extent/target
            // changes and framebuffer feedback within the same draw call.
            actual.assign(actual.size(),0x80203040); expected=actual;
            SpriteCommand textured{{0,0,8,8,0,1,0x80808080,0}};
            auto& d=textured.draw;
            d.texture_base=256; d.texture_pitch=2;
            d.texture_width=d.texture_height=16; d.flags=1|4|8|64;
            d.dx=d.dy=128; d.du=d.dv=128;
            textured.palette.fill(0x80604020);
            auto recolored=textured; recolored.palette.fill(0x80102070);
            auto taller=recolored; taller.draw.top=32; taller.draw.bottom=40;
            auto target=textured; target.draw.base=128;
            compare(std::array{textured,textured,recolored,recolored,taller,taller,target,target,textured});
            auto feedback=textured;
            feedback.draw.flags=1|4|8; feedback.draw.texture_base=0; feedback.draw.texture_pitch=1;
            feedback.draw.u0=16; // Read the preceding draw's neighboring pixel.
            compare(std::array{feedback,feedback,feedback});
        }
        compare(std::array{clear,quad});
        quad.draw.mask=0xff00ff00; compare(std::array{clear,quad}); quad.draw.mask=0;
        quad.draw.flags=256|alpha_blend_flag; quad.draw.blend=0x44;
        compare(std::array{clear,quad});
        // GS coordinates past framebuffer pitch address the next 32-line
        // page row. Preserve those writes and destination blending instead of
        // clipping them at the edge of the native image.
        for (unsigned pitch:{1U,2U,10U}) for (unsigned base:{0U,16352U}) {
            const unsigned width=pitch*64;
            for (unsigned left:{width-3,width,width*2+5}) {
                actual.assign(actual.size(),0x80202020); expected=actual;
                SpriteCommand wrapped{{left,29,left+10,40,base,pitch,0x40808080,0}};
                wrapped.draw.flags=256|alpha_blend_flag; wrapped.draw.blend=0x44;
                compare(std::array{wrapped});
                for (unsigned y=29;y<40;++y) for (unsigned x=left;x<left+10;++x)
                    if (actual[rgba_word(base,pitch,x,y)]!=0x40505050)
                        throw std::runtime_error("Wrapped sprite lost a blended pixel");
                auto overlap=wrapped;
                overlap.draw.left=left%width; overlap.draw.right=overlap.draw.left+2;
                overlap.draw.top+=left/width*32; overlap.draw.bottom+=left/width*32;
                overlap.draw.mask=0xff00ff00; overlap.draw.color=0x40706050;
                compare(std::array{wrapped,overlap,wrapped});
            }
        }
        actual.assign(actual.size(),0); expected=actual;
        for (unsigned method=0;method<8;++method) for (unsigned failure=0;failure<4;++failure) {
            quad.draw.test=1|(method<<1)|(128<<4)|(failure<<12);
            compare(std::array{clear,quad});
        }
        quad.draw.test=0; quad.draw.flags=depth_test_flag|depth_write_flag;
        quad.draw.depth_base=32; quad.draw.depth=0xffffff01;
        for (unsigned method=0;method<4;++method) {
            quad.draw.test=method<<17; compare(std::array{clear,quad});
        }
        for (unsigned failure:{1U,3U}) {
            quad.draw.test=1U|(failure<<12)|(1U<<17);
            compare(std::array{clear,quad});
        }
        quad.draw.test=3U<<17;
        // Exact 32-bit integer depth, including adjacent values beyond float precision.
        auto closer=quad; closer.draw.depth++; closer.draw.test=3<<17; closer.draw.color=0x80808080;
        compare(std::array{quad,closer,quad});
        for (unsigned y=0;y<32;++y) for (unsigned x=0;x<64;++x)
            actual[rgba_word(64,1,x,y)]=expected[rgba_word(64,1,x,y)]=0x80000000U|x*3|(y*5<<8)|((x+y)<<16);
        SpriteCommand texture{{0,0,16,16,0,1,0x80604020,0}};
        texture.draw.texture_base=64; texture.draw.texture_pitch=1;
        texture.draw.texture_width=64; texture.draw.texture_height=32;
        texture.draw.dx=texture.draw.dy=256; texture.draw.du=texture.draw.dv=256;
        for (unsigned function=0;function<4;++function) for (unsigned filter=0;filter<2;++filter) {
            texture.draw.flags=1|4|(function<<3)|(filter<<7); compare(std::array{clear,texture});
        }
        // Texture conversion preserves packed/indexed addressing across the
        // end of source storage, with a non-page-aligned base and palette data.
        for (unsigned indexed:{0U,1U,2U}) {
            for (unsigned word=0;word<actual.size();++word)
                actual[word]=expected[word]=0x80123456U^(word*0x1020305U);
            auto wrapped=texture;
            wrapped.draw.base=4096; wrapped.draw.texture_base=16380;
            wrapped.draw.texture_pitch=2; wrapped.draw.texture_width=128; wrapped.draw.texture_height=128;
            wrapped.draw.flags=1|4|8|(indexed ? 64U : 0U)|(indexed==2 ? indexed8_flag : 0U);
            wrapped.draw.du=wrapped.draw.dv=128*16;
            for (unsigned n=0;n<256;++n) wrapped.palette[n]=0x80000000U|n*0x10101U;
            compare(std::array{wrapped});
        }
        // Framebuffer textures remain native image snapshots, including overlap.
        texture.draw.texture_base=0; texture.draw.du=-256; texture.draw.u0=256;
        compare(std::array{clear,texture});
        // Logical texture size may exceed the resident framebuffer; only a
        // proven sampled footprint can use the smaller native snapshot.
        auto oversized=texture; oversized.draw.base=64;
        oversized.draw.texture_width=oversized.draw.texture_height=1024;
        oversized.draw.flags|=128; oversized.draw.clamp_u=oversized.draw.clamp_v=1;
        compare(std::array{clear,oversized});
        oversized.draw.clamp_u=0; oversized.draw.u0=0; // Negative filtered neighbor wraps to 1023.
        compare(std::array{clear,oversized});
        SpriteCommand triangle{{0,0,24,24,0,1,0,0}};
        triangle.draw.flags=triangle_flag|shaded_triangle_flag;
        triangle.triangle={RasterVertex{16,16,0x80302010},RasterVertex{320,32,0x80604020},RasterVertex{32,320,0x80806040}};
        compare(std::array{clear,triangle});
        triangle.draw.flags|=coverage_flag; compare(std::array{clear,triangle});
        triangle.draw.flags|=depth_test_flag|depth_write_flag; triangle.draw.depth_base=32; triangle.draw.test=1<<17;
        triangle.triangle[0].z=0x1000001; triangle.triangle[1].z=0x1000003; triangle.triangle[2].z=0x1000005;
        compare(std::array{clear,triangle});
        // Exercise corrected interpolation quotients across broad interiors,
        // fractional edges and varying colour/UV ranges against integer division.
        for (unsigned i=0;i<12;++i) {
            auto t=triangle;
            t.draw.flags=triangle_flag|shaded_triangle_flag|coverage_flag|1|4|8;
            t.draw.pitch=2; t.draw.right=t.draw.bottom=128;
            t.draw.texture_base=64; t.draw.texture_pitch=1;
            t.draw.texture_width=64; t.draw.texture_height=32;
            t.triangle={RasterVertex{int(i),int(i*3),0x803f207f,0,0},
                RasterVertex{2017-int(i*7),int(17+i),0x8011ff01,997,0},
                RasterVertex{int(31+i),2011-int(i*11),0x80ff3173,0,499}};
            compare(std::array{t});
        }
        // Masked resource imports preserve dirty native colour contents.
        const std::array<PixelWrite,2> writes{{{0,0x55,0xff},{0,0x6600,0xff00}}};
        native.write(writes); reference.write(writes); native.read(actual); reference.read(expected);
        if (actual!=expected) throw std::runtime_error("Native resource write coherence differs");
        NativeDrawing nd(true),rd(true);
        Packet packet{{0x8007,0x10000000,14,0},{1,0,0x1a,0},{6,0,0,0},{1<<16,0,0x4c,0},
            {31<<16,31<<16,0x40,0},{0x80402010,0,1,0},{0,0,5,0},{256|(256<<16),0,5,0}};
        nd.submit(native,std::array{packet}); rd.submit(reference,std::array{packet});
        if (nd.pixels()!=rd.pixels()) throw std::runtime_error("Native packet pipeline differs");
        {
            const auto ordinary=nd.pixels();
            auto retained_aa=packet; retained_aa[2][0]|=128;
            nd.submit(native,std::array{retained_aa}); rd.submit(reference,std::array{retained_aa});
            if (nd.pixels()!=ordinary || rd.pixels()!=ordinary)
                throw std::runtime_error("Retained AA1 changed sprite coverage or alpha");
        }
        // Cross the CPU command-batch bound. A FINISH in the middle and a
        // subsequent overlapping write must retain their original ordering.
        {
            auto many=packet;
            for (unsigned i=0;i<300;++i) {
                many.push_back({0x80000000U+i,0,1,0});
                many.push_back({0,0,5,0});
                many.push_back({256|(256<<16),0,5,0});
                if (i==270) many.push_back({0,0,0x61,0});
            }
            many[0][0]=0x8000U|unsigned(many.size()-1);
            nd.submit(native,std::array{many}); rd.submit(reference,std::array{many});
            if (nd.pixels()!=rd.pixels() || nd.completed_finishes()!=rd.completed_finishes())
                throw std::runtime_error("CPU draw batches changed packet or FINISH ordering");
        }
        {
            std::vector<SpriteCommand> scene;
            for (unsigned i=0;i<1024;++i) {
                auto t=triangle;
                t.draw.flags=triangle_flag|shaded_triangle_flag|coverage_flag;
                const auto x=int(i%16)*16,y=int((i/16)%16)*16;
                for (auto& v:t.triangle) {v.x+=x;v.y+=y;}
                t.draw.right=t.draw.bottom=40;
                scene.push_back(t);
            }
            native.upload(actual); reference.upload(actual);
            auto begin=std::chrono::steady_clock::now(); native.draw(scene); native.synchronize();
            const auto native_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
            begin=std::chrono::steady_clock::now(); reference.draw(scene); reference.synchronize();
            const auto reference_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
            native.read(actual); reference.read(expected);
            if (actual!=expected) throw std::runtime_error("Native batch benchmark changed pixels");
            std::cout<<"triangles="<<scene.size()<<" native_ms="<<native_ms<<" reference_compute_ms="<<reference_ms<<'\n';
        }
        // Disjoint screen regions share a dependency epoch; overlapping draws
        // must still observe earlier colour and depth writes.
        {
            actual.assign(actual.size(),0); expected=actual;
            std::vector<SpriteCommand> tiled;
            for (unsigned i=0;i<64;++i) {
                const unsigned x=(i%8)*16,y=(i/8)*16;
                tiled.push_back({{x,y,x+8,y+8,0,2,0x80102030U+i,0}});
            }
            auto overlap=tiled.back(); overlap.draw.mask=0xff00ff00;
            overlap.draw.color=0x80706050; tiled.push_back(overlap);
            const auto before=native.statistics().feedback_barriers;
            const auto before_calls=native.statistics().draw_calls;
            const auto before_batched=native.statistics().batched_primitives;
            compare(tiled);
            const auto barriers=native.statistics().feedback_barriers-before;
            if (native.statistics().ordered_fragment_access ? barriers!=0 : (!barriers || barriers>=tiled.size()/2))
                throw std::runtime_error("Disjoint draws did not batch attachment dependencies");
            if (native.statistics().draw_calls-before_calls>=tiled.size()/2 ||
                native.statistics().batched_primitives==before_batched)
                throw std::runtime_error("Compatible disjoint primitives did not share native draw calls");
        }
        // Overlapping primitives retain exact programmable depth/alpha/blend
        // order even when one hardware draw contains the entire batch.
        {
            actual.assign(actual.size(),0); expected=actual;
            std::vector<SpriteCommand> scene;
            for (unsigned i=0;i<160;++i) {
                SpriteCommand t{{0,0,32,32,0,1,0,0}};
                t.draw.flags=triangle_flag|shaded_triangle_flag|depth_test_flag|depth_write_flag|256|alpha_blend_flag;
                t.draw.depth_base=32; t.draw.blend=0x44;
                t.draw.test=((i%4)<<17)|1|((i%8)<<1)|(64<<4)|((i%4)<<12);
                t.draw.mask=(i%3==0) ? 0xff00ff00 : 0;
                const auto color=0x80000000U|(i*0x112233U&0xffffff);
                t.triangle={RasterVertex{0,0,color},RasterVertex{512,0,color},RasterVertex{0,512,color}};
                for (unsigned v=0;v<3;++v) t.triangle[v].z=0x80000000U+i*37+v*101;
                scene.push_back(t);
            }
            const auto calls=native.statistics().draw_calls;
            compare(scene);
            if (native.statistics().ordered_fragment_access && native.statistics().draw_calls-calls!=1)
                throw std::runtime_error("Ordered overlapping primitives did not share a hardware draw");
        }
        // Per-instance clipping must survive batching with a full-target scissor.
        // Alternate primitive topology and AA state, then overlap masked output.
        {
            actual.assign(actual.size(),0); expected=actual;
            std::vector<SpriteCommand> scene;
            for (unsigned i=0;i<16;++i) {
                auto t=triangle; const int x=int(i%4)*32,y=int(i/4)*32;
                t.draw={unsigned(x+4),unsigned(y+4),unsigned(x+20),unsigned(y+20),0,2,0x80403020,0};
                t.draw.flags=triangle_flag|shaded_triangle_flag|(i>=8 ? coverage_flag : 0);
                t.triangle={RasterVertex{x*16,y*16,0x80702010},RasterVertex{(x+30)*16,y*16,0x80702010},RasterVertex{x*16,(y+30)*16,0x80702010}};
                scene.push_back(t);
            }
            scene.push_back({{0,0,128,128,0,2,0x80007700,0xffff00ff}});
            compare(scene);
        }
        // Force reuse of all three draw-record slots while prior work is queued.
        {
            actual.assign(actual.size(),0); native.upload(actual);
            std::vector<SpriteCommand> scene;
            for (unsigned i=0;i<17000;++i) scene.push_back({{0,0,1,1,0,1,0x80000000U+i,0}});
            native.draw(scene); native.read(actual);
            if (actual[0]!=0x80000000U+16999)
                throw std::runtime_error("Native instance records reused before GPU completion");
        }
        // Repeated texture changes exceed the upload arena. Every queued draw
        // must sample its own version, including after the arena wraps.
        {
            actual.assign(actual.size(),0); native.upload(actual);
            SpriteCommand tile{{0,0,1,1,0,1,0x80808080,0}};
            tile.draw.flags=1|4|8; tile.draw.texture_base=64;
            tile.draw.texture_pitch=16; tile.draw.texture_width=1024; tile.draw.texture_height=256;
            tile.draw.dx=tile.draw.dy=16;
            const auto before=native.statistics();
            for (unsigned i=0;i<70;++i) {
                const PixelWrite write{rgba_word(64,16,0,0),0x80000001U+i,0xffffffffU};
                native.write(std::span(&write,1));
                tile.draw.left=i%64; tile.draw.top=i/64;
                tile.draw.right=tile.draw.left+1; tile.draw.bottom=tile.draw.top+1;
                native.draw(std::span(&tile,1));
            }
            native.read(actual);
            for (unsigned i=0;i<70;++i) if (actual[rgba_word(0,1,i%64,i/64)]!=0x80000001U+i)
                throw std::runtime_error("Upload arena overwrote a pending texture version");
            const auto& after=native.statistics();
            if (after.upload_bytes-before.upload_bytes<70*1024*1024 || after.submissions-before.submissions>=10)
                throw std::runtime_error("Upload arena did not retain bounded batched transfers");
        }
        // Deferred FINISH submits native work and publishes only retired queue
        // tokens. Exercise slot reuse, then verify completion and resulting pixels.
        {
            NativeDrawing asynchronous(true,NativeDrawing::Completion::deferred);
            auto batch=packet; batch[0][0]=0x8008;
            batch.push_back({0,0,0x61,0});
            NativeDrawingWorker worker(asynchronous,native);
            for (unsigned i=0;i<32;++i) {
                batch[5][0]=0x80000000U+i;
                worker.submit(std::span(&batch,1));
                batch[5][0]=0x8000ffffU; // Reuse producer storage immediately.
            }
            worker.wait();
            native.synchronize(); asynchronous.poll_completions(native);
            if (asynchronous.completed_finishes()!=32 || asynchronous.pixels()[0]!=0x8000001fU)
                throw std::runtime_error("Deferred FINISH lost completion or draw ordering");
            std::uint64_t token=0;
            for (unsigned i=0;i<20;++i) {
                const PixelWrite write{rgba_word(64,1,0,0),0x80000040U+i,0xffffffffU};
                native.write(std::span(&write,1));
                auto sample=texture; sample.draw.texture_base=64; sample.draw.du=sample.draw.dv=0;
                sample.draw.u0=sample.draw.v0=0; sample.draw.flags=1|4|8;
                native.draw(std::span(&sample,1));
                token=native.enqueue_finish();
                if (native.completed_submission()>token) throw std::runtime_error("Invalid queue completion token");
            }
            // FINISH must make progress with only nonblocking completion polls;
            // no synchronize/read call may be needed to drive the recording owner.
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while (native.completed_submission()<token && std::chrono::steady_clock::now()<deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (native.completed_submission()<token) throw std::runtime_error("Native render worker did not retire FINISH");
            native.read(actual);
            if (native.completed_submission()<token || actual[0]!=0x80000053U)
                throw std::runtime_error("Deferred texture upload slots were reused before retirement");
        }
        if (native.statistics().llvm_texture_kernels) {
            const auto before=native.statistics().texture_kernel_verified;
            native.verify_texture_kernels(true);
            const PixelWrite write{rgba_word(64,1,0,0),0x80654321U,0xffffffffU};
            native.write(std::span(&write,1));
            auto sample=texture; sample.draw.texture_base=64; sample.draw.du=sample.draw.dv=0;
            sample.draw.u0=sample.draw.v0=0; sample.draw.flags=1|4|8;
            native.draw(std::span(&sample,1));
            native.read(actual);
            if (native.statistics().texture_kernel_verified!=before+1 || actual[0]!=write.value)
                throw std::runtime_error("Explicit LLVM texture verification did not preserve the draw");
            native.verify_texture_kernels(false);
        }
        // More than the old 128-entry working set must retain native images.
        // A later source edit still invalidates precisely the affected views.
        {
            std::fill(actual.begin(),actual.end(),0);
            for (unsigned i=0;i<300;++i) actual[rgba_word(64+i*32,1,0,0)]=0x80001000U+i;
            native.upload(actual);
            auto tile=texture; tile.draw.flags=1|4|8;
            tile.draw.texture_pitch=1; tile.draw.texture_width=tile.draw.texture_height=8;
            tile.draw.u0=tile.draw.v0=0; tile.draw.du=tile.draw.dv=0;
            tile.draw.dx=tile.draw.dy=16;
            const auto draw_tiles=[&] {
                for (unsigned i=0;i<300;++i) {
                    tile.draw.texture_base=64+i*32;
                    tile.draw.left=i%64; tile.draw.top=i/64;
                    tile.draw.right=tile.draw.left+1; tile.draw.bottom=tile.draw.top+1;
                    native.draw(std::span(&tile,1));
                }
            };
            draw_tiles();
            const auto misses=native.statistics().texture_misses;
            draw_tiles(); native.read(actual);
            if (native.statistics().texture_misses!=misses)
                throw std::runtime_error("Stable texture working set was discarded");
            for (unsigned i=0;i<300;++i) if (actual[rgba_word(0,1,i%64,i/64)]!=0x80001000U+i)
                throw std::runtime_error("Retained texture cache changed pixels");
            const PixelWrite edit{rgba_word(64+149*32,1,0,0),0x80abcdef,0xffffffff};
            native.write(std::span(&edit,1)); draw_tiles(); native.read(actual);
            if (actual[rgba_word(0,1,149%64,149/64)]!=edit.value)
                throw std::runtime_error("Retained texture ignored source edit");
        }
        {
            NativeDrawing rejected(true);
            NativeDrawingWorker worker(rejected,native);
            const Packet malformed{{0x8001,0x10000000,14,0},{0,0,0x63,0}};
            worker.submit(std::span(&malformed,1));
            bool failed=false;
            try { worker.wait(); } catch (const std::invalid_argument&) { failed=true; }
            if (!failed || rejected.submitted_draws()) throw std::runtime_error("Drawing worker hid an unsupported packet");
            failed=false;
            try { (void)worker.poll(); } catch (const std::invalid_argument&) { failed=true; }
            if (!failed) throw std::runtime_error("Failed drawing worker reported success");
        }
        indexed8_textures(native,true);
        if (native.validation_errors()) throw std::runtime_error(native.validation_message());
        if (reference.validation_errors()) throw std::runtime_error(reference.validation_message());
        if (argc>1 && !native.statistics().texture_kernel_dispatches)
            throw std::runtime_error("No LLVM texture kernel was executed");
        std::cout<<"llvm_texture_kernel_dispatches="<<native.statistics().texture_kernel_dispatches<<'\n';
        std::cout<<"native_graphics_cases="<<checks<<" hardware triangles, AA, integer depth, blend, textures and resource coherence verified\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
