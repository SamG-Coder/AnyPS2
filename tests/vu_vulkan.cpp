#include <nt/ee_vu_launch.hpp>
#include <nt/native_drawing_worker.hpp>
#include <thread>
#include <nt/native_scanout.hpp>
#include <nt/ee_graphics.hpp>
#include <iostream>

extern "C" const nt::vu::Block* nt_vu_graphics_end_blocks();
extern "C" std::uint32_t nt_vu_graphics_end_count();
extern "C" const std::uint8_t* nt_vu_graphics_end_image();
extern "C" std::uint32_t nt_vu_graphics_end_size();

namespace {
void depth_and_coverage(nt::graphics::VulkanRenderer& renderer) {
    using namespace nt::graphics;
    const auto ad=[](Packet& p,unsigned reg,std::uint64_t value) {
        p.push_back({std::uint32_t(value),std::uint32_t(value>>32),reg,0});
    };
    const auto finish=[](Packet& p) { p[0]={0x8000U|unsigned(p.size()-1),0x10000000,14,0}; };
    const auto setup=[&](unsigned primitive,unsigned method,unsigned failure=0,bool alpha=false) {
        Packet p(1); ad(p,0x1a,1); ad(p,0,primitive); ad(p,0x4c,1ULL<<16);
        ad(p,0x40,(3ULL<<16)|(3ULL<<48)); ad(p,0x46,1); ad(p,0x4e,1);
        ad(p,0x47,0x10000U|(method<<17)|(alpha ? 1U|(6U<<1)|(128U<<4)|(failure<<12) : 0));
        ad(p,0x42,0x44); return p;
    };
    const auto sprite=[&](Packet& p,unsigned color,unsigned depth) {
        ad(p,1,color); ad(p,5,std::uint64_t(depth)<<32);
        ad(p,5,64ULL|(64ULL<<16)|(std::uint64_t(depth)<<32));
    };
    const auto pixel=[](const NativeDrawing& d) { return d.pixels()[rgba_word(0,1,1,1)]; };
    const auto zpixel=[](const NativeDrawing& d) { return d.pixels()[rgba_word(32,1,1,1)^0x600]; };
    // An FBW=0 frame scissored to rows 0-31 addresses like an FBW=32 frame;
    // a taller scissor would wrap rows and stops with a diagnostic.
    {
        NativeDrawing zero,wide;
        auto p=setup(6,1); ad(p,0x4c,0); ad(p,0x40,(31ULL<<16)|(15ULL<<48)); sprite(p,0x80402010,5); finish(p);
        auto q=setup(6,1); ad(q,0x4c,32ULL<<16); ad(q,0x40,(31ULL<<16)|(15ULL<<48)); sprite(q,0x80402010,5); finish(q);
        zero.submit(renderer,std::array{p}); wide.submit(renderer,std::array{q});
        if (zero.pixels()!=wide.pixels() || zero.pixels()[rgba_word(0,32,1,1)]!=0x80402010U)
            throw std::runtime_error("Zero-width frame did not draw as a 2048-pixel strip");
        // Rows 32-35 of an untextured sprite wrap onto rows 0-3, drawn after rows 28-31.
        NativeDrawing wrapped;
        auto tall=setup(6,1); ad(tall,0x4c,0); ad(tall,0x40,(31ULL<<16)|(40ULL<<48));
        ad(tall,1,0x80402010); ad(tall,5,(28ULL*16)<<16); ad(tall,5,64ULL|((36ULL*16)<<16)); finish(tall);
        wrapped.submit(renderer,std::array{tall});
        if (wrapped.pixels()[rgba_word(0,32,1,1)]!=0x80402010U || wrapped.pixels()[rgba_word(0,32,1,29)]!=0x80402010U ||
            wrapped.pixels()[rgba_word(0,32,1,5)])
            throw std::runtime_error("Zero-width frame rows did not wrap onto the 32-row strip");
        auto triangle=setup(3,1); ad(triangle,0x4c,0); ad(triangle,0x40,(31ULL<<16)|(40ULL<<48));
        ad(triangle,5,0); ad(triangle,5,64); ad(triangle,5,(64ULL<<16)); finish(triangle);
        bool stopped=false;
        try { wrapped.submit(renderer,std::array{triangle}); } catch (const std::invalid_argument&) { stopped=true; }
        if (!stopped) throw std::runtime_error("Triangle on a wrapping zero-width frame did not stop");
    }
    // Color drawn through a Z24 frame format lands in the depth page layout
    // with the top byte preserved; a PSMZ24 texture reads it back from there.
    {
        NativeDrawing d;
        auto p=setup(6,1); ad(p,0x4c,(1ULL<<16)|(0x31ULL<<24)); sprite(p,0x80402010,0); finish(p);
        d.submit(renderer,std::array{p});
        if (d.pixels()[rgba_word(0,1,1,1)^0x600]!=0x00402010U || d.pixels()[rgba_word(0,1,1,1)])
            throw std::runtime_error("Z24 frame color was not written in the depth page layout");
        auto t=setup(6,1); ad(t,0x4c,(1ULL<<16)|2ULL); // CT32 target at block 64
        ad(t,6,(1ULL<<14)|(0x31ULL<<20)|(2ULL<<26)|(2ULL<<30)|(1ULL<<35)); // TEX0: base 0, PSMZ24, 4x4, DECAL
        ad(t,0,6|(1U<<4)|(1U<<8)); // sprite, textured, UV coordinates
        ad(t,1,0x80000000); ad(t,3,(8ULL<<4)|(8ULL<<20)); ad(t,5,0);
        ad(t,3,(40ULL<<4)|(40ULL<<20)); ad(t,5,64ULL|(64ULL<<16));
        finish(t); d.submit(renderer,std::array{t});
        if ((d.pixels()[rgba_word(64,1,1,1)]&0xffffffU)!=0x402010U)
            throw std::runtime_error("PSMZ24 texture did not sample the depth page layout");
    }
    for (unsigned method=0;method<4;++method) {
        NativeDrawing d;
        auto p=setup(6,1); sprite(p,0x80402010,20); finish(p); d.submit(renderer,std::array{p});
        p=setup(6,method); sprite(p,0x80808080,10); finish(p); d.submit(renderer,std::array{p});
        if (pixel(d)!=(method==1 ? 0x80808080U : 0x80402010U) || zpixel(d)!=(method==1 ? 10U : 20U))
            throw std::runtime_error("Native depth comparison or write differs");
        p=setup(6,method); sprite(p,0x80776655,20); finish(p); d.submit(renderer,std::array{p});
        if (pixel(d)!=((method==1 || method==2) ? 0x80776655U : 0x80402010U))
            throw std::runtime_error("Native depth equality differs");
        p=setup(6,1); ad(p,0x4c,0xffffffff00010000ULL); sprite(p,0,30); finish(p); d.submit(renderer,std::array{p});
        if (zpixel(d)!=30) throw std::runtime_error("Color-masked depth-only draw was discarded");
    }
    for (unsigned failure=0;failure<4;++failure) {
        NativeDrawing d;
        auto p=setup(6,1); sprite(p,0x80402010,10); finish(p); d.submit(renderer,std::array{p});
        p=setup(6,2,failure,true); sprite(p,0x10776655,20); finish(p); d.submit(renderer,std::array{p});
        const unsigned colors[]{0x80402010,0x10776655,0x80402010,0x80776655};
        if (pixel(d)!=colors[failure] || zpixel(d)!=(failure==2 ? 20U : 10U))
            throw std::runtime_error("Alpha-test failure did not preserve independent color/depth masks");
    }
    NativeDrawing packed;
    auto p=setup(6,1); ad(p,1,0x80402010); finish(p); p[0][0]&=~0x8000U;
    p.push_back({0x8002,0x10000000,5,0});
    p.push_back({0,0,0xf1234567,0}); p.push_back({64,64,0xf1234567,0});
    packed.submit(renderer,std::array{p});
    if (zpixel(packed)!=0xf1234567) throw std::runtime_error("Packed XYZ lost unsigned 32-bit depth");
    p=setup(6,1); ad(p,0x4e,1ULL|(1ULL<<24)); finish(p); p[0][0]&=~0x8000U;
    p.push_back({0x8002,0x10000000,4,0});
    p.push_back({0,0,0x1234560,0}); p.push_back({64,64,0x1234560,0});
    packed.submit(renderer,std::array{p});
    if (zpixel(packed)!=0xf1123456) throw std::runtime_error("Packed XYZF or 24-bit depth preservation differs");
    const auto old=packed.pixels();
    p=setup(6,1); ad(p,0x4e,0); sprite(p,1,1); finish(p);
    bool rejected=false;
    try { packed.submit(renderer,std::array{p}); } catch (const std::runtime_error&) { rejected=true; }
    if (!rejected || packed.pixels()!=old) throw std::runtime_error("Aliased color/depth surfaces were published");
    NativeDrawing triangle;
    p=setup(3,1); ad(p,1,0x80402010);
    ad(p,5,10ULL<<32); ad(p,5,64ULL|(30ULL<<32)); ad(p,5,(64ULL<<16)|(50ULL<<32)); finish(p);
    triangle.submit(renderer,std::array{p});
    if (zpixel(triangle)!=25) throw std::runtime_error("Triangle depth interpolation differs");
    NativeDrawing aa;
    p=setup(3|128,1); ad(p,1,0x80808080);
    ad(p,5,10ULL<<32); ad(p,5,64ULL|(10ULL<<32)); ad(p,5,(64ULL<<16)|(10ULL<<32)); finish(p);
    aa.submit(renderer,std::array{p});
    // Integer pixel (2,1) is an interior sample, even though its surrounding
    // area crosses the edge. It must stay opaque and update depth.
    if (aa.pixels()[rgba_word(0,1,2,1)]!=0x80808080 || aa.pixels()[rgba_word(32,1,2,1)^0x600]!=10)
        throw std::runtime_error("Antialiased triangle interior lost opacity or depth");
    p=setup(3|128,3); ad(p,1,0x80000080);
    ad(p,5,5ULL<<32); ad(p,5,64ULL|(5ULL<<32)); ad(p,5,(64ULL<<16)|(5ULL<<32)); finish(p);
    aa.submit(renderer,std::array{p});
    if (aa.pixels()[rgba_word(0,1,2,1)]!=0x80808080)
        throw std::runtime_error("Farther geometry leaked through antialiased interior");
    NativeDrawing exterior;
    p=setup(3|128,1); ad(p,1,0x80808080);
    ad(p,5,8ULL|(8ULL<<16)|(10ULL<<32));
    ad(p,5,72ULL|(8ULL<<16)|(10ULL<<32));
    ad(p,5,8ULL|(72ULL<<16)|(10ULL<<32)); finish(p);
    exterior.submit(renderer,std::array{p});
    const auto edge=exterior.pixels()[rgba_word(0,1,0,1)];
    if ((edge>>24)==0 || (edge>>24)>=128 || exterior.pixels()[rgba_word(32,1,0,1)^0x600]!=0)
        throw std::runtime_error("Antialiased exterior lost partial coverage or wrote depth");

}

void triangles(nt::graphics::VulkanRenderer& renderer) {
    using nt::graphics::Packet;
    using nt::graphics::RasterVertex;
    const auto ad=[](Packet& p,unsigned reg,std::uint64_t value) {
        p.push_back({std::uint32_t(value),std::uint32_t(value>>32),reg,0});
    };
    const auto finish=[](Packet& p) { p[0]={0x8000U|unsigned(p.size()-1),0x10000000,14,0}; };
    const auto setup=[&](unsigned primitive) {
        Packet p(1); ad(p,0x1a,1); ad(p,0,primitive); ad(p,0x4c,1ULL<<16);
        ad(p,0x40,(3ULL<<16)|(3ULL<<48)); ad(p,0x46,1);
        ad(p,0x42,104ULL|(128ULL<<32)); return p;
    };
    const auto vertex=[&](Packet& p,RasterVertex v,bool skip=false) {
        ad(p,1,v.color|(std::uint64_t(std::bit_cast<std::uint32_t>(v.q))<<32));
        ad(p,2,std::bit_cast<std::uint32_t>(v.s)|(std::uint64_t(std::bit_cast<std::uint32_t>(v.t))<<32));
        ad(p,3,v.u|(std::uint64_t(v.v)<<16));
        ad(p,skip ? 13 : 5,unsigned(v.x)|(std::uint64_t(v.y)<<16));
    };
    const auto pixel=[](const auto& drawing,unsigned x,unsigned y) {
        return drawing.pixels()[nt::graphics::rgba_word(0,1,x,y)];
    };
    const RasterVertex a{0,0,0x80000010},b{64,0,0x80000010},c{0,64,0x80000010},d{64,64,0x80000010};
    for (unsigned primitive:{3U,4U,5U}) {
        nt::graphics::NativeDrawing drawing;
        auto first=setup(primitive|64);
        vertex(first,a); vertex(first,b); finish(first);
        drawing.submit(renderer,std::array{first});
        if (drawing.submitted_draws()) throw std::runtime_error("Incomplete triangle produced pixels");
        Packet second(1);
        if (primitive==3) {
            vertex(second,c); vertex(second,b); vertex(second,d); vertex(second,c);
        } else if (primitive==4) { vertex(second,c); vertex(second,d); }
        else { vertex(second,d); vertex(second,c); }
        finish(second); drawing.submit(renderer,std::array{second});
        if (drawing.submitted_draws()!=2) throw std::runtime_error("Triangle assembly lost a primitive across packets");
        for (unsigned y=0;y<4;++y) for (unsigned x=0;x<4;++x)
            if (pixel(drawing,x,y)!=0x80000010) throw std::runtime_error("Triangle edge ownership left a gap or blended twice");
    }
    nt::graphics::NativeDrawing shaded;
    auto p=setup(3|8);
    vertex(p,{0,0,0x80000080}); vertex(p,{0,64,0x80800000}); vertex(p,{64,0,0x80008000});
    finish(p); shaded.submit(renderer,std::array{p});
    if (pixel(shaded,0,0)!=0x80000080 || pixel(shaded,1,1)!=0x80202040 || pixel(shaded,2,2))
        throw std::runtime_error("Triangle winding, interpolated color or bottom/right exclusion differs");
    nt::graphics::NativeDrawing suppressed;
    p=setup(4); vertex(p,a); vertex(p,b); vertex(p,c,true); vertex(p,d); finish(p);
    suppressed.submit(renderer,std::array{p});
    if (suppressed.submitted_draws()!=1 || pixel(suppressed,0,0) || pixel(suppressed,3,3)!=d.color)
        throw std::runtime_error("Suppressed triangle kick lost strip continuity");
    // A later invalid graphics state must not publish any earlier draws or state.
    auto good=setup(3); vertex(good,a); vertex(good,b); vertex(good,c); finish(good);
    auto bad=setup(3); ad(bad,0x22,1); vertex(bad,a); vertex(bad,b); vertex(bad,c); finish(bad);
    const auto before=suppressed.pixels(); bool rejected=false;
    try { suppressed.submit(renderer,std::array{good,bad}); }
    catch (const std::invalid_argument&) { rejected=true; }
    if (!rejected || suppressed.pixels()!=before || suppressed.submitted_draws()!=1)
        throw std::runtime_error("Invalid graphics state published a partial batch");
    nt::graphics::NativeDrawing clipped;
    p=setup(3); ad(p,0x18,16ULL|(16ULL<<32));
    ad(p,0x40,1ULL|(2ULL<<16)|(1ULL<<32)|(2ULL<<48));
    vertex(p,{0,0,1}); vertex(p,{112,0,2}); vertex(p,{0,112,0x80443322}); finish(p);
    clipped.submit(renderer,std::array{p});
    if (pixel(clipped,0,0) || pixel(clipped,1,1)!=0x80443322 || pixel(clipped,3,1))
        throw std::runtime_error("Triangle scissor, offset or last-vertex flat color differs");
    // Sub-pixel XYOFFSET jitter keeps the established offset; a whole-pixel
    // change moves the drawing.
    const auto offset_triangle=[&](std::initializer_list<std::uint64_t> offsets) {
        auto drawing=std::make_unique<nt::graphics::NativeDrawing>();
        auto q=setup(3);
        for (const auto offset:offsets) ad(q,0x18,offset);
        vertex(q,{0,0,1}); vertex(q,{112,0,2}); vertex(q,{0,112,0x80443322}); finish(q);
        drawing->submit(renderer,std::array{q});
        return drawing->pixels();
    };
    const auto at=[](std::uint64_t x,std::uint64_t y) { return x|(y<<32); };
    const auto settled=offset_triangle({at(16,16)});
    // Choose a sub-pixel offset that changes this triangle's coverage when it
    // is the only offset written, so the jitter check below is meaningful.
    std::uint64_t jitter=0;
    for (std::uint64_t dy=1;dy<16 && !jitter;++dy)
        for (std::uint64_t dx=0;dx<16 && !jitter;++dx)
            if (offset_triangle({at(16-dx,16+dy)})!=settled) jitter=at(16-dx,16+dy);
    if (!jitter) throw std::runtime_error("No sub-pixel offset changes the fixture's coverage");
    if (offset_triangle({at(16,16),jitter})!=settled)
        throw std::runtime_error("Sub-pixel XYOFFSET jitter moved the drawing");
    if (offset_triangle({at(16,16),at(16,32)})!=offset_triangle({at(16,32)}) || offset_triangle({at(16,32)})==settled)
        throw std::runtime_error("A whole-pixel XYOFFSET change did not move the drawing");
    nt::graphics::NativeDrawing textured;
    Packet upload(1);
    ad(upload,0x50,(32ULL<<32)|(1ULL<<48)); ad(upload,0x51,0);
    ad(upload,0x52,4ULL|(4ULL<<32)); ad(upload,0x53,0); finish(upload); upload[0][0]&=~0x8000U;
    upload.push_back({0x8004,2U<<26,0,0});
    for (unsigned y=0;y<4;++y) {
        nt::graphics::Quadword row{};
        for (unsigned x=0;x<4;++x) row[x]=0x80000000U|(x+16*y);
        upload.push_back(row);
    }
    textured.submit(renderer,std::array{upload});
    p=setup(4|16|256);
    ad(p,6,32ULL|(1ULL<<14)|(2ULL<<26)|(2ULL<<30)|(1ULL<<34)|(1ULL<<35));
    vertex(p,{0,0,0,0,0}); vertex(p,{64,0,0,64,0});
    vertex(p,{0,64,0,0,64}); vertex(p,{64,64,0,64,64}); finish(p);
    textured.submit(renderer,std::array{p});
    for (unsigned y=0;y<4;++y) for (unsigned x=0;x<4;++x)
        if (pixel(textured,x,y)!=(0x80000000U|(x+16*y)))
            throw std::runtime_error("Fixed-UV triangle sampling differs across the shared edge");
    const std::array<RasterVertex,4> perspective{{{0,0,0,0,0,0,0,1},{64,0,0,0,0,0.25f,0,0.25f},
        {0,64,0,0,0,0,1,1},{64,64,0,0,0,0.25f,0.25f,0.25f}}};
    p=setup(4|16);
    for (const auto& v:perspective) vertex(p,v);
    finish(p); textured.submit(renderer,std::array{p});
    if (pixel(textured,2,1)!=0x80000010 || pixel(textured,3,3)!=0x80000031)
        throw std::runtime_error("ST/Q triangle sampling omitted perspective division");
    const auto perspective_pixels=textured.pixels();
    p=setup(4|16); finish(p); p[0][0]&=~0x8000U;
    p.push_back({0x8004,3U<<28,0x512,0});
    for (const auto& v:perspective) {
        p.push_back({std::bit_cast<std::uint32_t>(v.s),std::bit_cast<std::uint32_t>(v.t),std::bit_cast<std::uint32_t>(v.q),0});
        p.push_back({0,0,0,0}); p.push_back({unsigned(v.x),unsigned(v.y),0,0});
    }
    textured.submit(renderer,std::array{p});
    if (textured.pixels()!=perspective_pixels) throw std::runtime_error("Packed STQ did not forward Q to the vertex");
    p=setup(4|16);
    for (auto v:perspective) { v.q=0; vertex(p,v); }
    finish(p); rejected=false;
    try { textured.submit(renderer,std::array{p}); } catch (const std::runtime_error&) { rejected=true; }
    if (!rejected || textured.pixels()!=perspective_pixels)
        throw std::runtime_error("Zero Q published a partial draw");
    if (renderer.validation_errors()) throw std::runtime_error("Triangle Vulkan validation error");
}
}

int main() {
    try {
        nt::vu::Binding binding(nt::vu::Program(1,{nt_vu_graphics_end_blocks(),nt_vu_graphics_end_count()}),0,
            {nt_vu_graphics_end_image(),nt_vu_graphics_end_size()});
        if (!binding.upload(1,std::array{nt::vu::UploadView{0,{nt_vu_graphics_end_image(),nt_vu_graphics_end_size()}}}))
            throw std::runtime_error("Synthetic graphics program binding failed");
        struct Consumer {
            nt::graphics::VulkanRenderer renderer{nullptr,true};
            nt::graphics::NativeDrawing drawing;
        } consumer;
        nt::ee::NativeVuLaunch launch(binding);
        launch.graphics={&consumer,[](void* raw,std::span<const nt::vu::GraphicsPacket> packets) {
            auto& sink=*static_cast<Consumer*>(raw);
            sink.drawing.submit(sink.renderer,packets); return true;
        }};
        launch.state.data[0]={0x8007,0x10000000,14,0};
        unsigned cursor=1;
        const auto ad=[&](unsigned reg,std::uint64_t value) {
            launch.state.data[cursor++]={std::uint32_t(value),std::uint32_t(value>>32),reg,0};
        };
        ad(0x1a,1); ad(0,6); ad(0x4c,1ULL<<16);
        ad(0x40,(3ULL<<16)|(3ULL<<48)); ad(1,0x80402010);
        ad(5,0); ad(5,32ULL|(32ULL<<16));
        const nt::ee::NativeLaunchView request{0,0,0};
        if (launch.submit(1,{},&request)!=nt::ee::Stop::budget || launch.state.integers[0]!=1 ||
            launch.state.graphics_pending || launch.last_result.blocks!=1)
            throw std::runtime_error("Compiled VU graphics launch did not complete");
        for (unsigned pixel=0;pixel<4;++pixel)
            if (consumer.drawing.pixels()[pixel]!=0x80402010)
                throw std::runtime_error("Compiled VU packet did not produce expected GPU pixels");
        if (consumer.drawing.pixels()[4] || consumer.renderer.validation_errors())
            throw std::runtime_error("Unexpected coverage or Vulkan validation failure");
        {
            nt::graphics::NativeDrawingWorker worker(consumer.drawing,consumer.renderer);
            auto asynchronous=std::make_unique<nt::ee::NativeVuLaunch>(binding);
            asynchronous->enable_async(); asynchronous->state=launch.state;
            asynchronous->state.integers[0]=0;
            asynchronous->graphics={&worker,[](void* raw,std::span<const nt::vu::GraphicsPacket> packets) {
                static_cast<nt::graphics::NativeDrawingWorker*>(raw)->submit(packets); return true;
            }};
            if (asynchronous->submit(1,{},&request)!=nt::ee::Stop::budget)
                throw std::runtime_error("Compiled asynchronous graphics was not accepted");
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while (asynchronous->pending()) {
                if (asynchronous->poll()!=nt::ee::Stop::budget || std::chrono::steady_clock::now()>deadline)
                    throw std::runtime_error("Compiled asynchronous graphics did not complete");
                std::this_thread::yield();
            }
            worker.wait();
            for (unsigned pixel=0;pixel<4;++pixel)
                if (consumer.drawing.pixels()[pixel]!=0x80402010)
                    throw std::runtime_error("Asynchronous compiled VU/drawing pipeline changed pixels");
            if (consumer.drawing.pixels()[4] || consumer.renderer.validation_errors())
                throw std::runtime_error("Asynchronous compiled graphics coverage or validation failed");
        }
        nt::ee::NativePacketSetup direct;
        direct.graphics=launch.graphics;
        std::vector<std::uint8_t> transport;
        const auto word=[&](std::uint32_t value) {
            for (unsigned byte=0;byte<4;++byte) transport.push_back(static_cast<std::uint8_t>(value>>(byte*8)));
        };
        const auto payload=[&]() {
            for (unsigned index=0;index<cursor;++index) for (const auto lane:launch.state.data[index]) word(lane);
        };
        launch.state.data[5][0]=0x80706050;
        word(0x0700002a); word(0x50000000|cursor); payload();
        if (direct.consume(1,transport,false)!=nt::ee::Stop::budget ||
            direct.read(0x10003c30).value!=42 || consumer.drawing.pixels()[0]!=0x80706050)
            throw std::runtime_error("VIF DIRECT did not reach native Vulkan drawing");
        const auto image=consumer.drawing.pixels();
        transport.clear(); word(0x0700002b); word(0x51000000|cursor); payload(); word(0x7b000000);
        if (direct.consume(1,transport,false)!=nt::ee::Stop::unsupported_system ||
            direct.read(0x10003c30).value!=42 || consumer.drawing.pixels()!=image)
            throw std::runtime_error("Invalid DIRECT batch published partial configuration or drawing");
        transport.clear(); word(0x50000000);
        if (direct.consume(1,transport,false)!=nt::ee::Stop::invalid_state || consumer.drawing.pixels()!=image)
            throw std::runtime_error("Encoded-zero DIRECT accepted truncated payload");
        launch.state.data[5][0]=0x80908070;
        transport.clear(); word(0x51000000|cursor); payload();
        if (direct.consume(1,transport,false)!=nt::ee::Stop::budget || consumer.drawing.pixels()[0]!=0x80908070)
            throw std::runtime_error("Serialized DIRECTHL failed");
        launch.state.data[5][0]=0x80201008;
        transport.clear(); payload();
        direct.write(0x10003000,8);
        if (direct.consume(2,transport,false)!=nt::ee::Stop::unsupported_system)
            throw std::runtime_error("Paused graphics accepted a native submission");
        direct.write(0x10003000,0);
        if (direct.consume(2,transport,false)!=nt::ee::Stop::budget || consumer.drawing.pixels()[0]!=0x80201008)
            throw std::runtime_error("Native GIF submission did not reach Vulkan");
        // One image packet split across transport submissions, including failure
        // and retry at completion. No pixels are published from the prefix.
        transport.clear();
        const auto quad=[&](std::uint64_t low,std::uint64_t high) {
            word(std::uint32_t(low)); word(std::uint32_t(low>>32));
            word(std::uint32_t(high)); word(std::uint32_t(high>>32));
        };
        quad(4ULL|(1ULL<<60),14);
        quad(1ULL<<48,0x50); quad(0,0x51); quad(4ULL|(1ULL<<32),0x52); quad(0,0x53);
        quad(0x8001ULL|(2ULL<<58),0);
        const auto before_stream=consumer.drawing.pixels();
        if (direct.consume(2,transport,false)!=nt::ee::Stop::budget || consumer.drawing.pixels()!=before_stream)
            throw std::runtime_error("Split image prefix was rejected or published early");
        if (direct.read(0x10003020).status!=nt::ee::Stop::unsupported_system)
            throw std::runtime_error("Pending stream reported idle GIF status");
        const std::vector<std::uint8_t> interleaved{0,0,0,0x11};
        if (direct.consume(1,interleaved,false)!=nt::ee::Stop::unsupported_system)
            throw std::runtime_error("Incomplete GIF packet allowed cross-path work");
        const auto sink=direct.graphics;
        direct.graphics.submit=[](void*,std::span<const nt::graphics::Packet>) { return false; };
        transport.clear(); quad(0x2222222211111111ULL,0x4444444433333333ULL);
        if (direct.consume(2,transport,false)!=nt::ee::Stop::unsupported_system || consumer.drawing.pixels()!=before_stream)
            throw std::runtime_error("Rejected stream completion committed state");
        direct.graphics=sink;
        if (direct.consume(2,transport,false)!=nt::ee::Stop::budget ||
            consumer.drawing.pixels()[0]!=0x11111111 || consumer.drawing.pixels()[5]!=0x44444444)
            throw std::runtime_error("Retried split image upload differs");
        transport.clear(); quad(0x8001ULL|(2ULL<<58),0);
        if (direct.consume(2,transport,false)!=nt::ee::Stop::budget)
            throw std::runtime_error("Pending image header rejected");
        direct.write(0x10003000,1); // Reset discards pending transport data.
        transport.clear(); payload();
        if (direct.consume(2,transport,false)!=nt::ee::Stop::budget || consumer.drawing.pixels()[0]!=0x80201008)
            throw std::runtime_error("GIF reset retained pending transport framing");
        transport.clear(); quad(0x7fffULL|(1ULL<<60),14);
        std::vector<std::uint8_t> oversized(65536*16);
        if (direct.consume(2,transport,false)!=nt::ee::Stop::budget ||
            direct.consume(2,oversized,false)!=nt::ee::Stop::unsupported_system)
            throw std::runtime_error("Graphics stream bound was not enforced");
        direct.write(0x10003000,1);
        if (consumer.renderer.validation_errors()) throw std::runtime_error("Native transfers caused Vulkan validation errors");
        direct.graphics.wait=[](void* raw) {
            static_cast<Consumer*>(raw)->renderer.enqueue_finish(); return true;
        };
        const auto before_flush=consumer.drawing.pixels();
        transport.clear(); word(0x11000000); word(0x13000000);
        if (direct.consume(1,transport,false)!=nt::ee::Stop::budget || consumer.drawing.pixels()!=before_flush ||
            consumer.renderer.validation_errors())
            throw std::runtime_error("Native Vulkan flush altered drawing or failed completion");
        // A flush after uncommitted DIRECT cannot be moved before that draw.
        transport.clear(); word(0x50000000|cursor); payload(); word(0x11000000);
        if (direct.consume(1,transport,false)!=nt::ee::Stop::unsupported_system || consumer.drawing.pixels()!=before_flush)
            throw std::runtime_error("Native flush was reordered across an uncommitted draw");
        direct.graphics.synchronous=true;
        const nt::graphics::Packet dither_setup{{0x8002,0x10000000,14,0},
            {0x76543210,0x01234567,0x44,0},{1,0,0x45,0}};
        consumer.drawing.submit(consumer.renderer,std::array{dither_setup});
        if (direct.consume(1,transport,false)!=nt::ee::Stop::budget ||
            consumer.drawing.pixels()[0]!=0x80201008 || consumer.renderer.validation_errors())
            throw std::runtime_error("Synchronous DIRECT/FLUSH batch did not finish real Vulkan work");
        const auto before_dither_rejection=consumer.drawing.pixels();
        const nt::graphics::Packet dithered_16{{0x8003,0x10000000,14,0},
            {(2U<<24)|(1U<<16),0,0x4c,0},{0,0,5,0},{32U|(32U<<16),0,5,0}};
        bool dither_rejected=false;
        try { consumer.drawing.submit(consumer.renderer,std::array{dithered_16}); }
        catch (const std::invalid_argument&) { dither_rejected=true; }
        if (!dither_rejected || consumer.drawing.pixels()!=before_dither_rejection)
            throw std::runtime_error("Unsupported dithered target published pixels");
        transport.clear(); word(0x50000000|cursor); payload(); word(0x11000000); word(0x7b000000);
        const auto committed=consumer.drawing.pixels();
        if (direct.consume(1,transport,false)!=nt::ee::Stop::unsupported_system || consumer.drawing.pixels()!=committed)
            throw std::runtime_error("Malformed trailing command partially published fenced graphics");

        nt::ee::NativeGraphics lifecycle;
        if (lifecycle.write(0x12000000,0x62)!=nt::ee::Stop::budget ||
            lifecycle.write(0x12000090,0x12345678)!=nt::ee::Stop::budget ||
            lifecycle.write(0x120000a0,0xabcdef)!=nt::ee::Stop::budget ||
            lifecycle.read(0x12000090).value!=0x12345678 || lifecycle.output().displays[1]!=0xabcdef ||
            lifecycle.write(0x120000e0,0xff123456)!=nt::ee::Stop::budget ||
            lifecycle.output().background!=0x123456)
            throw std::runtime_error("Native output selection lost display configuration");
        lifecycle.context=&consumer;
        lifecycle.synchronize=[](void* raw) {
            static_cast<Consumer*>(raw)->renderer.synchronize(); return nt::ee::Stop::budget;
        };
        lifecycle.reset_drawing=[](void* raw) {
            static_cast<Consumer*>(raw)->drawing.reset(); return nt::ee::Stop::budget;
        };
        const nt::graphics::Packet upload_setup{{0x8004,0x10000000,14,0},
            {0,1U<<16,0x50,0},{0,0,0x51,0},{8,1,0x52,0},{0,0,0x53,0}};
        consumer.drawing.submit(consumer.renderer,std::array{upload_setup});
        const auto before_reset=consumer.drawing.pixels();
        if (lifecycle.configure(1,2,1)!=nt::ee::Stop::budget ||
            lifecycle.write(0x12001000,0x200)!=nt::ee::Stop::budget || lifecycle.mode() ||
            lifecycle.resets()!=1 || lifecycle.output().control || lifecycle.output().buffers[1] ||
            consumer.drawing.pixels()!=before_reset)
            throw std::runtime_error("Native graphics reset did not preserve image storage");
        const nt::graphics::Packet stale_image{{0x8001,0x08000000,0,0},{1,2,3,4}};
        bool rejected=false;
        try { consumer.drawing.submit(consumer.renderer,std::array{stale_image}); }
        catch (const std::invalid_argument&) { rejected=true; }
        if (!rejected || consumer.drawing.pixels()!=before_reset)
            throw std::runtime_error("Native reset retained an active image upload");
        const nt::graphics::Packet stale_draw{{0x8002,0x10000000,14,0},{0,0,5,0},{32U|(32U<<16),0,5,0}};
        rejected=false;
        try { consumer.drawing.submit(consumer.renderer,std::array{stale_draw}); }
        catch (const std::invalid_argument&) { rejected=true; }
        if (!rejected) throw std::runtime_error("Native reset retained drawing configuration");
        launch.state.integers[0]=0; // Re-submit the packet at its original data offset.
        if (launch.submit(1,{},&request)!=nt::ee::Stop::budget || consumer.drawing.pixels()[0]!=0x80201008)
            throw std::runtime_error("Compiled VU drawing did not recover after reset");
        nt::graphics::NativeDrawing masked;
        const auto masked_packet=[](unsigned mask,unsigned color) {
            return nt::graphics::Packet{{0x800a,0x10000000,14,0},
                {1,0,0x1a,0},{14,0,0,0},{1U<<16,0,0x4c,0},
                {3U<<16,3U<<16,0x40,0},{mask,0,0x22,0},
                {color^0xffffffU,0,1,0},{0,0,5,0},{color,0,1,0},
                {64U|(64U<<16),0,4,0},{0,0,0x3f,0}};
        };
        masked.submit(consumer.renderer,std::array{masked_packet(2,0xff112233)});
        for (unsigned y=0;y<4;++y) for (unsigned x=0;x<4;++x)
            if (masked.pixels()[nt::graphics::rgba_word(0,1,x,y)]!=(y&1 ? 0xff112233U : 0U))
                throw std::runtime_error("Even scanline suppression or flat sprite color differs");
        masked.submit(consumer.renderer,std::array{masked_packet(3,0xff445566)});
        for (unsigned y=0;y<4;++y) for (unsigned x=0;x<4;++x)
            if (masked.pixels()[nt::graphics::rgba_word(0,1,x,y)]!=(y&1 ? 0xff112233U : 0xff445566U))
                throw std::runtime_error("Odd scanline suppression changed preserved rows");
        const auto mask_before=masked.pixels();
        rejected=false;
        try { masked.submit(consumer.renderer,std::array{masked_packet(1,0xffffffff)}); }
        catch (const std::invalid_argument&) { rejected=true; }
        if (!rejected || masked.pixels()!=mask_before || masked.submitted_draws()!=2)
            throw std::runtime_error("Reserved scanline mode committed drawing");
        masked.submit(consumer.renderer,std::array{masked_packet(0,0xff778899)});
        for (unsigned y=0;y<4;++y) for (unsigned x=0;x<4;++x)
            if (masked.pixels()[nt::graphics::rgba_word(0,1,x,y)]!=0xff778899)
                throw std::runtime_error("Scanline mask disable did not restore full coverage");
        const auto empty_before=consumer.drawing.submitted_draws();
        if (direct.consume(2,{},false)!=nt::ee::Stop::budget ||
            consumer.drawing.submitted_draws()!=empty_before)
            throw std::runtime_error("Empty GIF submission invented drawing work");
        {
            const nt::graphics::Packet finish{{0x8001,0x10000000,14,0},{0,0,0x61,0}};
            const auto before=masked.completed_finishes();
            masked.submit(consumer.renderer,std::array{finish});
            if (masked.completed_finishes()!=before+1)
                throw std::runtime_error("Native drawing finish fence was not completed");
            auto malformed=finish; malformed[0][0]=0x8002; malformed.push_back({0,0,0x63,0});
            rejected=false;
            try { masked.submit(consumer.renderer,std::array{malformed}); }
            catch (const std::invalid_argument&) { rejected=true; }
            if (!rejected || masked.completed_finishes()!=before+1)
                throw std::runtime_error("Rejected packet published a finish fence");
            nt::ee::NativeGraphics graphics;
            if (graphics.write(0x12001010,0x7d00)!=nt::ee::Stop::budget)
                throw std::runtime_error("Native finish event could not be unmasked");
            graphics.drawing_completed();
            if (!(graphics.read(0x12001000).value&2) || graphics.poll()!=1)
                throw std::runtime_error("Native finish event did not reach the interrupt owner");
            graphics.write(0x12001000,2);
            if (graphics.read(0x12001000).value&2)
                throw std::runtime_error("Native finish event acknowledgement failed");
        }
        const auto tex0=(1ULL<<14)|(2ULL<<26)|(2ULL<<30)|(1ULL<<34)|(1ULL<<35);
        const nt::graphics::Packet texture_update{{0x800a,0x10000000,14,0},
            {0x116,0,0,0},{(1U<<16)|8U,0,0x4c,0},
            {std::uint32_t(tex0),std::uint32_t(tex0>>32),6,0},{0,0,0x16,0},
            {123,456,0x34,0},{789,123,0x36,0},
            {0,0,3,0},{0,0,5,0},{64U|(64U<<16),0,3,0},{64U|(64U<<16),0,5,0}};
        masked.submit(consumer.renderer,std::array{texture_update});
        for (unsigned y=0;y<4;++y) for (unsigned x=0;x<4;++x)
            if (masked.pixels()[nt::graphics::rgba_word(256,1,x,y)]!=0xff778899)
                throw std::runtime_error("TEX2 changed preserved texture layout or mip setup changed base sampling");
        const std::array<std::uint64_t,2> display_buffers{0,8U|(1U<<9)};
        const std::array<std::uint64_t,2> display_extents{0,(3ULL<<32)|(3ULL<<44)};
        const auto frame=nt::graphics::scanout(masked.pixels(),2,display_buffers,display_extents);
        if (!frame || frame->width!=4 || frame->height!=4 ||
            !std::all_of(frame->pixels.begin(),frame->pixels.end(),[](auto p) { return p==0xff778899; }) ||
            nt::graphics::scanout(masked.pixels(),0,display_buffers,display_extents))
            throw std::runtime_error("Scanout did not use the selected game's surface and extent");
        rejected=false;
        try { nt::graphics::scanout(masked.pixels(),3,display_buffers,display_extents); }
        catch (const std::invalid_argument&) { rejected=true; }
        if (!rejected) throw std::runtime_error("Unsupported dual-circuit composition was hidden");
        {
            std::vector<std::uint32_t> pixels(1024*1024);
            pixels[nt::graphics::rgba_word(0,1,0,0)]=0xff0000ff;
            pixels[nt::graphics::rgba_word(0,1,0,1)]=0xffff0000;
            const std::array<std::uint64_t,2> buffers{1U<<9,(1U<<9)|(1ULL<<43)};
            const std::array<std::uint64_t,2> extents{1ULL<<44,0};
            const auto filtered=nt::graphics::scanout(pixels,35|(112U<<8),buffers,extents);
            if (!filtered || filtered->width!=1 || filtered->height!=2 ||
                filtered->pixels!=std::vector<std::uint32_t>{0xff8f0070,0xff700000})
                throw std::runtime_error("Adjacent-row constant-alpha display composition differs");
            const auto first=nt::graphics::scanout(pixels,35|(255U<<8),buffers,extents);
            const auto second=nt::graphics::scanout(pixels,35,buffers,extents);
            const auto background=nt::graphics::scanout(pixels,35|128,buffers,extents,false,0x00ff00);
            if (first->pixels!=std::vector<std::uint32_t>{0xff0000ff,0xffff0000} ||
                second->pixels!=std::vector<std::uint32_t>{0xffff0000,0xff000000} ||
                background->pixels!=std::vector<std::uint32_t>{0xff00ff00,0xff00ff00})
                throw std::runtime_error("Display blend endpoints or background differ");
        }
        // Alternate visible rows and put unrelated data immediately after a
        // half-height target. Frame-mode scanout must never display that data.
        masked.submit(consumer.renderer,std::array{masked_packet(2,0xff112233),masked_packet(3,0xff445566)});
        const std::array<std::uint64_t,2> field_buffers{(1U<<9)|(1ULL<<43),0};
        const std::array<std::uint64_t,2> field_extents{(3ULL<<32)|(4ULL<<44),0};
        const auto half=nt::graphics::scanout(masked.pixels(),1,field_buffers,field_extents,true);
        if (!half || half->width!=4 || half->height!=5)
            throw std::runtime_error("Interlaced output changed the requested display extent");
        for (unsigned y=0;y<5;++y) for (unsigned x=0;x<4;++x)
            if (half->pixels[y*4+x]!=(y/2==1 ? 0xff445566U : 0xff112233U))
                throw std::runtime_error("Half-height scanout read past the source rows or lost the display origin");
        const auto full=nt::graphics::scanout(masked.pixels(),1,field_buffers,field_extents);
        if (!full || full->pixels[4]!=0xff445566 || full->pixels[12]!=0xff000000)
            throw std::runtime_error("Full-height scanout unexpectedly duplicated source rows");
        triangles(consumer.renderer);
        depth_and_coverage(consumer.renderer);
        std::cout<<"Compiled VU to Vulkan drawing integration passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
