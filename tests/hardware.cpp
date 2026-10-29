#include <nt/system_bus.hpp>
#include <nt/kernel.hpp>
#include <iostream>

namespace {
void check(bool v, const char* message) { if (!v) throw std::runtime_error(message); }
template<class F> void rejects(F function) {
    bool rejected = false;
    try { function(); } catch (const nt::runtime::MemoryFault&) { rejected = true; }
    check(rejected,"Unsupported device operation accepted");
}
}
void gs_initialization() {
    nt::hardware::SystemBus bus;
    constexpr auto csr = 0x12001000U, imr = csr+16;
    check(bus.load(csr,8) == 0x551b4000 && bus.load(imr,8) == 0x7f00,"GS reset identity/mask differs");
    bus.store(csr,8,0x11f); // Acknowledge events and flush the empty FIFO.
    check(bus.load(csr,8) == 0x551b4000,"GS command bits leaked into status");
    bus.store(0xb2000080,8,0x123456789abcdef0ULL);
    bus.store(0x12000084,4,0x10203040);
    check(bus.load(0x12000080,8) == 0x102030409abcdef0ULL,"GS display register lanes/alias differ");
    nt::kernel::Kernel kernel;
    nt::runtime::State state;
    state.gpr[3].lo = 0xffffff8fU; state.gpr[4].lo = 0xffffffffffffe000ULL;
    check(kernel.service(state,bus) && bus.load(imr,8) == 0x6000,"GsPutIMR mask differs");
    state.gpr[3].lo = 0x70;
    check(kernel.service(state,bus) && state.gpr[2].lo == 0x6000,"GsGetIMR differs from MMIO");
    bus.advance_cycles(100);
    check(!bus.interrupt_pending() && bus.load(csr,8) == 0x551b4000,"GS invented video events");
    rejects([&] { bus.store(csr+0x40,8,1); });
    rejects([&] { bus.store(0x120000d0,8,1); });
    rejects([&] { bus.load(csr+0x20,8); });
    rejects([&] { bus.store(csr+1,8,0); });
    rejects([&] { bus.load(csr+8,8); });
    rejects([&] { bus.load_quad(csr); });
    rejects([&] { bus.store_quad(csr,{}); });
    check(bus.load(csr+0x40,8) == 0 && bus.load(0x120000d0,8) == 0,"Rejected GS transfer mutated state");
    bus.store(csr+1,1,2); // RESET byte lane.
    check(bus.load(csr,8) == 0x551b4000 && bus.load(imr,8) == 0x7f00 &&
          bus.load(0x12000080,8) == 0,"GS reset did not restore initialization state");
}
void gs_scanout() {
    nt::hardware::SystemBus bus;
    nt::kernel::Kernel kernel;
    nt::runtime::State state;
    state.set(3,2); state.set(4,1); state.set(5,2); state.set(6,1);
    check(kernel.service(state,bus) && bus.load(0x12000020,8) == 3,"SetGsCrt did not configure SMODE2");
    const auto edge = [](unsigned half_lines) { return (294912000ULL*1001*half_lines+31499999)/31500000; };
    bus.store(0x1000f010,4,13); // GS, VBlank start/end.
    bus.advance_cycles(edge(480)-1);
    check(bus.load(0x1000f000,4) == 0 && !(bus.load(0x12001000,8)&8),"VBlank occurred early");
    bus.advance_cycles(1);
    check(bus.load(0x1000f000,4) == 4 && bus.interrupt_pending(),"VBlank start missing");
    bus.store(0x1000f000,4,4);
    bus.advance_cycles(edge(487)-edge(480));
    check((bus.load(0x12001000,8)&0x2008) == 0x2008 && !bus.interrupt_pending(),"Masked GS field event differs");
    bus.store(0x12001010,8,0x7700); // Unmask already pending VSINT.
    check(bus.load(0x1000f000,4) == 1,"Pending GS event did not interrupt on unmask");
    bus.store(0x1000f000,4,1);
    bus.advance_cycles(1);
    check(!bus.interrupt_pending(),"Latched GS event repeatedly triggered INTC");
    bus.store(0x12001000,8,8);
    bus.advance_cycles(edge(525)-edge(487)-1);
    check(bus.load(0x1000f000,4) == 8 && !(bus.load(0x12001000,8)&8),"VBlank end or CSR acknowledgement differs");
    bus.store(0x1000f000,4,8);
    bus.advance_cycles(edge(1012)-edge(525));
    check(!(bus.load(0x12001000,8)&0x2000) && (bus.load(0x1000f000,4)&1),"Second field did not toggle/rearm GS interrupt");
    state.set(5,99);
    rejects([&] { kernel.service(state,bus); });
    check(bus.load(0x12000020,8) == 3,"Rejected CRT mode mutated state");
    rejects([&] { bus.store(0x12000020,8,0); });
    bus.store(0x12001000,8,0x200);
    bus.store(0x1000f000,4,15);
    bus.advance_cycles(edge(525));
    check(!bus.interrupt_pending(),"Reset left scanout running");

    // Batched versus fragmented PAL timing: preserve phase across field boundaries.
    nt::hardware::Gs batch, split;
    batch.set_crt(1,3,0); split.set_crt(1,3,0);
    constexpr auto field_cycles = 5898240ULL;
    batch.advance_cycles(field_cycles*3);
    for (unsigned i=0;i<3*1024;++i) split.advance_cycles(field_cycles/1024);
    check(batch.read(0x12001000,8) == split.read(0x12001000,8) &&
          batch.take_interrupts() == 12 && split.take_interrupts() == 12,"PAL chunking changed field phase/events");
    batch.write(0x12001000,8,31);
    batch.advance_cycles(5435817); // One cycle before PAL half-line 576.
    check(batch.take_interrupts() == 0,"PAL blanking edge occurred early");
    batch.advance_cycles(1);
    check(batch.take_interrupts() == 4,"PAL blanking edge missing");
}
void vif_initialization() {
    nt::hardware::SystemBus bus;
    for (const auto base : {0x10003800U,0x10003c00U}) {
        check(bus.load(base,4) == 0 && bus.load(base+0x60,4) == 0,"VIF cold state is not idle");
        bus.store(base+0x20,4,~0U);
        bus.store(base+0x30,4,0x12345678);
        bus.store(base+0x50,4,2);
        bus.store(base+0x70,4,0x12345678);
        for (unsigned lane=0;lane<8;++lane) bus.store(base+0x100+lane*16,4,0x12340000+lane);
        bus.store(base+0x10,4,6);
        check(bus.load(base,4) == 0x300 && bus.load(base+0x10,4) == 0,"VIF stop/break strobes differ");
        bus.store(base+0x10,4,8);
        check(bus.load(base,4) == 0 && bus.load(base+0x20,4) == 7,"VIF cancel damaged configuration");
        bus.store(base+0x10,4,7);
        check(bus.load(base,4) == 0 && bus.load(base+0x20,4) == 0 &&
              bus.load(base+0x30,4) == 0x5678 && bus.load(base+0x50,4) == 2 &&
              bus.load(base+0x70,4) == 0x12345678,"VIF soft reset/configuration differs");
        for (unsigned lane=0;lane<8;++lane)
            check(bus.load(base+0x100+lane*16,4) == 0x12340000+lane,"VIF reset lost row/column data");
        bus.store(base+0x10,4,2);
        bus.store(base,4,0); // Read-only status cannot clear a force break.
        check(bus.load(base,4) == 0x200,"VIF STAT write modified read-only flags");
        bus.store(base+0x10,4,8);
        rejects([&] { bus.store(base+0x10,8,1); });
        rejects([&] { bus.store(base+0x14,4,1); });
        rejects([&] { bus.store(base+0x80,4,1); });
        rejects([&] { bus.load(base+0x180,4); });
    }
    bus.store(0x90003830,4,0xabcd);
    check(bus.load(0x10003830,4) == 0xabcd && bus.load(0x10003c30,4) == 0x5678,"VIF units/aliases are not isolated");
    rejects([&] { bus.store(0x10003c00,4,1U<<23); });
    rejects([&] { bus.load(0x100038a0,4); });
    for (const auto fifo : {0x10004000U,0x10005000U}) {
        rejects([&] { bus.store(fifo,4,0); });
        rejects([&] { bus.store_quad(fifo,{}); });
        rejects([&] { bus.load_quad(fifo); });
    }
    check(bus.load(0x10003c00,4) == 0 && !bus.interrupt_pending(),"Rejected VIF operation changed idle state");
}
void gif_initialization() {
    nt::hardware::SystemBus bus;
    constexpr auto base = 0x10003000U;
    for (const auto reg : {0U,0x10U,0x20U,0x40U,0x50U,0x60U,0x70U,0x80U,0x90U,0xa0U})
        check(bus.load(base+reg,4) == 0,"GIF did not start idle");
    bus.store(0xb0003010,4,~0U);
    bus.store(base,4,0xfffffffeU);
    check(bus.load(base,4) == 8 && bus.load(base+16,4) == 5 && bus.load(base+32,4) == 13,
          "GIF pause/mask/intermittent readback or aliases differ");
    bus.store(base,4,0);
    check(bus.load(base+32,4) == 5,"GIF unpause erased mode");
    bus.store(0x12000080,8,0x123456789abcdef0ULL);
    bus.store(0x1000e020,4,0x7fff7fff);
    check(bus.load(0x1000e020,4) == 0x03ff03ff,"D_PCR writable fields differ");
    rejects([&] { bus.store(0x1000e020,4,0x80000000); });
    for (const auto channel : {0x10008000U,0x10009000U,0x1000a000U}) {
        bus.store(channel,4,0x44);
        bus.store(channel+16,4,0x1000+(channel&0xf000));
        bus.store(channel+32,4,0x12345);
        bus.store(channel+48,4,0x2000);
        bus.store(channel+64,4,0x3000); bus.store(channel+80,4,0x4000);
        bus.store(channel+128,4,0);
        rejects([&] { bus.store(channel,4,0x144); });
        rejects([&] { bus.store(channel+128,4,16); });
        rejects([&] { bus.store(channel+96,4,0); });
        rejects([&] { bus.store(channel,8,0); });
        bus.store(base,4,9); // Reset takes precedence over pause, preserves DMA setup.
        check(bus.load(channel,4) == 0x44 && bus.load(channel+32,4) == 0x2345 &&
              bus.load(channel+16,4) == 0x1000+(channel&0xf000) && bus.load(channel+128,4) == 0,
              "GIF reset/rejected DMA start changed DMA configuration");
    }
    check(bus.load(base,4) == 0 && bus.load(base+16,4) == 0 && bus.load(base+32,4) == 0 &&
          bus.load(0x12000080,8) == 0x123456789abcdef0ULL && bus.load(0x1000e020,4) == 0x03ff03ff,
          "GIF reset failed or changed GS/DMAC state");
    rejects([&] { bus.store(base+32,4,0xffffffff); });
    rejects([&] { bus.load(base+48,4); });
    rejects([&] { bus.store(base,8,1); });
    rejects([&] { bus.store(base+4,4,1); });
    rejects([&] { bus.store(0x10006000,4,0); });
    rejects([&] { bus.store_quad(0x10006000,{}); });
    rejects([&] { bus.load_quad(0x10006000); });
    bus.advance_cycles(100);
    check(bus.load(base+32,4) == 0 && bus.load(0x1000e010,4) == 0 && !bus.interrupt_pending(),
          "Initialization or rejected transfer fabricated completion");
}
void vif_dma_transport() {
    nt::hardware::SystemBus bus;
    bus.store_quad(0x1000,{0x0100010100000000ULL,0x0700005505000002ULL});
    bus.store(0x10008010,4,0x1000); bus.store(0x10008020,4,1);
    bus.store(0x10008000,4,0x101);
    bus.advance_cycles(5);
    check(bus.load(0x10008020,4)==1 && bus.load(0x1000e010,4)==0,"Disabled DMA consumed payload");
    bus.store(0x1000e000,4,1); bus.store(0x1000f590,4,0x10000);
    bus.advance_cycles(5);
    check(bus.load(0x10008020,4)==1,"Suspended DMA consumed payload");
    bus.store(0x1000f590,4,0); bus.store(0x1000e010,4,1U<<16);
    bus.advance_cycles(1);
    check(bus.load(0x10008020,4)==0 && bus.load(0x10008010,4)==0x1010 &&
          !(bus.load(0x10008000,4)&0x100) && bus.interrupt_lines()==0x800 &&
          bus.load(0x10003840,4)==0x101 && bus.load(0x10003850,4)==2 && bus.load(0x10003830,4)==0x55,
          "VIF normal DMA configuration/completion differs");
    bus.store(0x1000e010,4,1);
    check(!bus.interrupt_pending(),"DMA completion acknowledgement failed");
    // TTE starts STROW in the tag; its payload spans both a tag and data beat.
    bus.store_quad(0x70000000,{0x70000001,0x1111111130000000ULL});
    bus.store_quad(0x70000010,{0x3333333322222222ULL,0x0000000044444444ULL});
    bus.store(0x10008030,4,0x80000000); bus.store(0x10008000,4,0x145);
    bus.advance_cycles(1);
    check(bus.load(0x10008020,4)==1 && !(bus.load(0x1000e010,4)&1),"DMA completed before chain payload");
    bus.advance_cycles(1);
    for (unsigned lane=0;lane<4;++lane)
        check(bus.load(0x10003900+lane*16,4)==0x11111111U*(lane+1),"Split STROW or scratchpad DMA differs");
    check((bus.load(0x1000e010,4)&1)!=0,"Chain completion missing");
    check(bus.load(0x10008030,4)==0x80000000,"END changed the tag address");
    // VIF1 NEXT -> REF -> CNT -> REFE, including zero-QWC tags and TIE.
    nt::hardware::SystemBus chain;
    chain.store(0x1000e000,4,1);
    chain.store_quad(0x3000,{0x0000310020000000ULL,0});
    chain.store_quad(0x3100,{0x0000400030000001ULL,0});
    chain.store_quad(0x3110,{0x10000001,0});
    chain.store_quad(0x3120,{0x0700002200000000ULL,0});
    chain.store_quad(0x3130,{0,0});
    chain.store_quad(0x4000,{0x0700001100000000ULL,0});
    chain.store(0x10009030,4,0x3000); chain.store(0x10009000,4,0x105);
    chain.advance_cycles(6);
    check(chain.load(0x10003c30,4)==0x22 && chain.load(0x10009030,4)==0x3140 &&
          chain.load(0x1000e010,4)==2 && !(chain.load(0x10009000,4)&0x100),"VIF1 source-chain traversal differs");
    chain.store(0x1000e010,4,2);
    chain.store_quad(0x5000,{0x90000000,0});
    chain.store(0x10009030,4,0x5000); chain.store(0x10009000,4,0x185);
    chain.advance_cycles(1);
    check(chain.load(0x1000e010,4)==2 && !(chain.load(0x10009000,4)&0x100),"TIE ignored tag IRQ");
    // Failed beat must not partially apply MARK before an unsupported command.
    bus.store(0x1000e010,4,1);
    bus.store_quad(0x2000,{0x1400000007000099ULL,0});
    bus.store(0x10008010,4,0x2000); bus.store(0x10008020,4,1); bus.store(0x10008000,4,0x101);
    rejects([&] { bus.advance_cycles(1); });
    check(bus.load(0x10003830,4)==0x55 && bus.load(0x10008020,4)==1 && !(bus.load(0x1000e010,4)&1),
          "Unsupported VIF beat changed registers or fabricated completion");
    nt::hardware::Vif vif;
    // Buffer-address setup is staged separately from VU launch state.
    const std::array<std::uint32_t,4> setup{0x0300ffff,0x02000423,0x04000555,0x03000412};
    vif.consume(1,setup);
    check(vif.read(0x10003ca0,4)==0x12 && vif.read(0x10003cb0,4)==0x23 &&
          vif.read(0x10003cc0,4)==0x3ff && vif.read(0x10003c90,4)==0x155 &&
          vif.read(0x10003cd0,4)==0 && vif.read(0x10003ce0,4)==0,
          "VIF address setup changed launch state or failed to latch BASE on OFFSET");
    rejects([&]{vif.consume(0,setup);});
    vif.consume(0,std::span(setup).subspan(2,1));
    check(vif.read(0x10003890,4)==0x155 && vif.read(0x100038d0,4)==0,
          "VIF0 ITOPS staging differs");
    const std::array<std::uint32_t,2> invalid_setup{0x03000077,0x14000000};
    rejects([&]{vif.consume(1,invalid_setup);});
    check(vif.read(0x10003ca0,4)==0x12,"Failed VIF beat partially changed BASE");
    const std::array<std::uint32_t,2> idle_flush{0x11000000,0x07000035};
    vif.consume(1,idle_flush);
    check(vif.read(0x10003c30,4)==0x35 && vif.read(0x10003ca0,4)==0x12,
          "Idle FLUSH failed to continue command processing");
    const std::array<std::uint32_t,2> first{0x4a0201ff,0x12345678};
    const std::array<std::uint32_t,3> second{0x23456789,0x3456789a,0x456789ab};
    vif.consume(0,first); vif.consume(0,second);
    check(vif.micro_word(0,1022)==first[1] && vif.micro_word(0,1023)==second[0] &&
          vif.micro_word(0,0)==second[1] && vif.micro_word(0,1)==second[2] && vif.read(0x10003860,4)==0,
          "Split MPG upload or micro-memory wrap differs");
}
void gif_packets() {
    nt::hardware::Gs clut;
    const auto load_palette=[&](std::uint32_t color) {
        clut.write_command(0x50,(64ULL<<32)|(1ULL<<48));
        clut.write_command(0x52,8ULL|(2ULL<<32)); clut.write_command(0x53,0);
        for(unsigned i=0;i<4;++i) {
            clut.upload({std::uint64_t(color)|(std::uint64_t(color+1)<<32),
                std::uint64_t(color+2)|(std::uint64_t(color+3)<<32)});
            color+=4;
        }
    };
    load_palette(0x10203040);
    const auto tex0=(20ULL<<20)|(64ULL<<37)|(3ULL<<56);
    clut.write_command(6,tex0|(2ULL<<61));
    for(unsigned i=0;i<16;++i)check(clut.palette_color(48+i)==0x10203040+i,"CSM1 palette order or CSA differs");
    load_palette(0x50607080);
    clut.write_command(7,tex0|(4ULL<<61));
    check(clut.palette_color(48)==0x10203040,"Conditional CLUT load ignored remembered base");
    clut.write_command(7,tex0|(1ULL<<61));
    check(clut.palette_color(48)==0x50607080,"Unconditional CLUT reload failed");
    clut.write_command(6,tex0); // CLD=0 keeps the loaded palette.
    check(clut.palette_color(48)==0x50607080,"CLD zero changed palette");
    clut.write_command(0x1c,1); // CSM2 row zero, eight initialized pixels then zeros.
    clut.write_command(6,tex0|(1ULL<<55)|(3ULL<<61));
    check(clut.palette_color(48)==0x50607080 && clut.palette_color(55)==0x50607087 &&
          clut.palette_color(56)==0,"CSM2 did not use linear CLUT coordinates");
    load_palette(0x90a0b0c0);
    clut.write_command(7,tex0|(1ULL<<55)|(5ULL<<61));
    check(clut.palette_color(48)==0x50607080,"CLUT second base comparison differs");
    nt::hardware::Gs texture;
    texture.write_command(0x50,(32ULL<<32)|(1ULL<<48));
    texture.write_command(0x52,2ULL|(2ULL<<32)); texture.write_command(0x53,0);
    texture.upload({0x4050607020102030ULL,0xc0d0e0f08090a0b0ULL});
    texture.write_command(0x1a,1); texture.write_command(0,6|0x110);
    texture.write_command(0x4c,1ULL<<16); texture.write_command(0x40,(1ULL<<16)|(1ULL<<48));
    texture.write_command(6,32ULL|(1ULL<<14)|(1ULL<<26)|(1ULL<<30)|(1ULL<<34)|(1ULL<<35));
    texture.write_command(3,0); texture.write_command(5,0);
    texture.write_command(3,32ULL|(32ULL<<16)); texture.write_command(5,0x00200020);
    check(texture.local_word(0)==0x20102030 && texture.local_word(3)==0xc0d0e0f0,
          "GS upload, TEX0/UV decoding and Vulkan texture delivery differ");
    texture.write_command(0x14,96); texture.write_command(8,5);
    texture.write_command(3,16ULL|(16ULL<<16)); texture.write_command(5,0);
    texture.write_command(3,48ULL|(48ULL<<16)); texture.write_command(5,0x00200020);
    check(texture.local_word(0)==0x68708090,"TEX1 bilinear selection or half-texel offset differs");
    nt::hardware::Gs pixels;
    pixels.write_command(0x1a,1);
    pixels.write_command(0,6);
    pixels.write_command(0x4c,1ULL<<16);
    pixels.write_command(0x40,(3ULL<<16)|(3ULL<<48));
    pixels.write_command(1,0x12345678);
    pixels.write_command(5,0);
    pixels.write_command(5,0x00200020);
    for (unsigned i=0;i<4;++i) check(pixels.local_word(i)==0x12345678,"Solid sprite pixel differs");
    check(pixels.local_word(4)==0,"Sprite exceeded exclusive right edge");
    // Force-alpha plus a framebuffer mask; the last vertex supplies flat colour.
    pixels.write_command(0x4a,1);
    pixels.write_command(0x4c,(0x0000ff00ULL<<32)|(1ULL<<16));
    pixels.write_command(1,0x00776655);
    pixels.write_command(0x18,16ULL|(16ULL<<32));
    pixels.write_command(5,0x00100010);
    pixels.write_command(5,0x00300030);
    check(pixels.local_word(0)==0x80775655,"Sprite offset, FBA or framebuffer mask differs");
    // PSMCT24 keeps the destination's high byte; scissor limits this draw to (1,1).
    pixels.write_command(0x4c,(1ULL<<24)|(1ULL<<16));
    pixels.write_command(0x40,1ULL|(1ULL<<16)|(1ULL<<32)|(1ULL<<48));
    pixels.write_command(1,0xabcdef01);
    pixels.write_command(5,0x00100010);
    pixels.write_command(5,0x00400040);
    check(pixels.local_word(3)==0x80cdef01 && pixels.local_word(0)==0x80775655,
          "PSMCT24 or inclusive scissor differs");
    // A copied GS transaction must not expose pixels from a later failed beat.
    pixels.write_command(0,6);
    pixels.write_command(5,0x00100010);
    pixels.write_command(1,0xffffffff);
    nt::hardware::Gif rollback;
    rollback.consume({(2ULL<<60)|(1ULL<<58)|0x8001,0xe5},pixels);
    rejects([&]{rollback.consume({0x00400040,0},pixels);});
    check(pixels.local_word(3)==0x80cdef01 && pixels.pending_vertices()==1,
          "Failed GIF beat leaked rasterized pixels or vertex state");
    auto copy=pixels;
    copy.write_command(5,0x00400040);
    check(copy.local_word(3)==0x80ffffff && pixels.local_word(3)==0x80cdef01,
          "GS pixel copy-on-write isolation failed");
    // Vertex assembly retains attributes, advances skipped strips/fans, and
    // never reports a completed draw before a rasterizer exists.
    for (unsigned primitive=0;primitive<7;++primitive) {
        nt::hardware::Gs assembly;
        constexpr std::array<unsigned,7> count{1,2,2,3,3,3,2};
        assembly.write_command(0,primitive);
        assembly.write_command(1,0x1122334455667788ULL);
        assembly.write_command(2,0x0123456789abcdefULL);
        assembly.write_command(3,0x12345678);
        for (unsigned i=0;i<count[primitive]-1;++i)
            assembly.write_command(4,0x7a12345600200010ULL+i);
        const auto pending=assembly.pending_vertices();
        rejects([&]{assembly.write_command(5,0xfedcba9800400030ULL);});
        check(assembly.pending_vertices()==pending,"Unsupported draw changed pending vertices");
        assembly.write_command(13,0xfedcba9800400030ULL);
        const auto retained=primitive==2 ? 1U : (primitive==4 || primitive==5 ? 2U : 0U);
        check(assembly.pending_vertices()==retained,"Skipped primitive topology differs");
        if (retained) {
            const auto& last=assembly.pending_vertex(retained-1);
            check(last.x==0x30 && last.y==0x40 && last.z==0xfedcba98 && last.fog==0x7a &&
                  last.rgbaq==0x1122334455667788ULL && last.st==0x0123456789abcdefULL && last.uv==0x12345678,
                  "Vertex attribute capture differs");
        }
        if (primitive==5) check(assembly.pending_vertex(0).x==0x10,"Triangle fan lost its anchor");
        assembly.write_command(0,primitive);
        check(!assembly.pending_vertices(),"PRIM write did not restart assembly");
    }
    // DIRECT may start at any word of a DMA beat. A complete GIF packet,
    // including its data, crosses three beats here.
    nt::hardware::SystemBus direct;
    direct.store(0x1000e000,4,1);
    direct.store_quad(0x6000,{0x0000800150000002ULL,0x0000000e10000000ULL});
    direct.store_quad(0x6010,{0x0000123400000000ULL,0x0000004c00000000ULL});
    direct.store_quad(0x6020,{0,0});
    direct.store(0x10009010,4,0x6000); direct.store(0x10009020,4,3);
    direct.store(0x10003000,4,8); // GIF pause must retain the VIF DMA beat.
    direct.store(0x10009000,4,0x101);
    direct.advance_cycles(2);
    check(direct.load(0x10009020,4)==2 && !(direct.load(0x1000e010,4)&2),
          "Paused DIRECT lost its partial quadword or completed DMA");
    direct.store(0x10003000,4,0);
    direct.advance_cycles(2);
    check(direct.load(0x10009020,4)==0 && (direct.load(0x1000e010,4)&2),
          "DIRECT DMA failed to complete after packet delivery");
    nt::hardware::Vif receiver;
    nt::hardware::Gif shared;
    nt::hardware::Gs target;
    shared.consume({(1ULL<<60)|0x8001,14},target); // Path 3 owns the packet.
    const std::array<std::uint32_t,1> command{0x50000002};
    const std::array<std::uint32_t,4> tag{0x8001,0x10000000,14,0};
    const std::array<std::uint32_t,4> data{0x5678,0,0x4c,0};
    check(receiver.consume(1,command,&shared,&target),"DIRECT setup rejected");
    check(!receiver.consume(1,tag,&shared,&target),"DIRECT interleaved an active path-3 packet");
    shared.consume({0x1234,0x4c},target);
    check(receiver.consume(1,tag,&shared,&target) && shared.busy(2) && !shared.accepts(3),
          "DIRECT did not acquire path 2 after path-3 EOP");
    check(receiver.consume(1,data,&shared,&target) && target.command_register(0x4c)==0x5678 &&
          shared.accepts(3),"DIRECT payload, retry state or EOP release differs");
    nt::hardware::Gs gs;
    nt::hardware::Gif gif;
    gif.consume({(1ULL<<60)|(1ULL<<46)|(3ULL<<47)|2,14},gs);
    gif.consume({0x1234,0x4c},gs); gif.consume({0x5678,0x4d},gs);
    check(gs.command_register(0)==3 && gs.command_register(0x4c)==0x1234 &&
          gs.command_register(0x4d)==0x5678 && !(gif.read(0x10003020,4)&0xc00),"Packed A_D setup differs");
    gif.consume({(3ULL<<60)|(1ULL<<58)|1,0x321},gs);
    gif.consume({0x11223344,0x55667788},gs); gif.consume({0x99aabbcc,~0ULL},gs);
    check(gs.command_register(1)==0x11223344 && gs.command_register(2)==0x55667788 &&
          gs.command_register(3)==0x99aabbcc,"Odd REGLIST padding was treated as a register");
    gs.write_command(0,0); // A point completes at the next XYZ2 write.
    gif.consume({(2ULL<<60)|(1ULL<<58)|1,0x51},gs);
    rejects([&]{gif.consume({7,9},gs);});
    check(gs.command_register(1)==0x11223344,"Failed REGLIST beat partially changed GS state");
    gif.write(0x10003000,4,1);
    check(gif.read(0x10003040,4)==0 && gs.command_register(1)==0x11223344,"GIF reset changed GS state");
    nt::hardware::SystemBus bus;
    bus.store(0x1000e000,4,1); bus.store(0x1000e010,4,1U<<18);
    bus.store(0x12001010,8,0); bus.store(0x1000f010,4,1);
    bus.store_quad(0x1000,{(1ULL<<60)|3,14});
    bus.store_quad(0x1010,{0xffffffff12345678ULL,0x60});
    bus.store_quad(0x1020,{0xffff0000abcdef12ULL,0x62});
    bus.store_quad(0x1030,{0,0x61});
    bus.store(0x1000a010,4,0x1000); bus.store(0x1000a020,4,4);
    bus.store(0x10003000,4,8); bus.store(0x1000a000,4,0x101);
    bus.advance_cycles(8);
    check(bus.load(0x1000a020,4)==4 && !(bus.load(0x1000e010,4)&4),"Paused GIF consumed DMA");
    bus.store(0x10003000,4,0); bus.advance_cycles(3);
    check(bus.load(0x1000a020,4)==1 && !(bus.load(0x1000e010,4)&4),"GIF DMA completed early");
    check(bus.load(0x12001080,8)==0xabcd000012345678ULL && (bus.load(0x12001000,8)&3)==1,
          "SIGNAL/LABEL mask or ordering differs");
    bus.advance_cycles(1);
    check((bus.load(0x12001000,8)&3)==3 && bus.interrupt_lines()==0xc00 &&
          bus.load(0x1000a020,4)==0 && !(bus.load(0x1000a000,4)&0x100),"GIF/GS finish or interrupt completion differs");
    bus.store(0x12001000,8,3); bus.store(0x1000f000,4,1); bus.store(0x1000e010,4,4);
    check(!bus.interrupt_pending(),"GIF/GS interrupt acknowledgement failed");
    bus.store_quad(0x2000,{(1ULL<<60)|1,14});
    bus.store_quad(0x2010,{1,0x53});
    bus.store(0x1000a010,4,0x2000); bus.store(0x1000a020,4,2); bus.store(0x1000a000,4,0x101);
    bus.advance_cycles(1);
    rejects([&]{bus.advance_cycles(1);});
    check(bus.load(0x1000a010,4)==0x2010 && bus.load(0x1000a020,4)==1 &&
          !(bus.load(0x1000e010,4)&4),"Unsupported GS upload consumed DMA or completed transfer");
    gif.write(0x10003000,4,1);
    gif.consume({1,0xffffffffffffffffULL},gs); // NREG=0 means sixteen NOP entries.
    for (unsigned i=0;i<15;++i) gif.consume({},gs);
    check((gif.read(0x10003020,4)&0xc00)!=0,"NREG zero did not select sixteen registers");
    gif.consume({},gs);
    check(!(gif.read(0x10003020,4)&0xc00),"Sixteen-register packet did not finish");
    gif.consume({(2ULL<<58)|1,0},gs);
    rejects([&]{gif.consume({},gs);});
    check((gif.read(0x10003020,4)&0xc00)!=0,"Invalid IMAGE payload consumed parser state");
}
void gs_image_uploads() {
    nt::hardware::Gs gs;
    nt::hardware::Gif gif;
    const auto setup=[&](unsigned format,unsigned base,unsigned x,unsigned y,unsigned width,unsigned height) {
        gs.write_command(0x50,(std::uint64_t(format)<<56)|(2ULL<<48)|(std::uint64_t(base)<<32));
        gs.write_command(0x51,(std::uint64_t(y)<<48)|(std::uint64_t(x)<<32));
        gs.write_command(0x52,(std::uint64_t(height)<<32)|width);
        gs.write_command(0x53,0);
    };
    setup(0,0,0,0,8,2);
    gif.consume({(2ULL<<58)|4,0},gs);
    for (unsigned n=0;n<16;n+=4)
        gif.consume({std::uint64_t(n+1)|(std::uint64_t(n+2)<<32),std::uint64_t(n+3)|(std::uint64_t(n+4)<<32)},gs);
    const std::array<unsigned,16> offsets{0,1,4,5,8,9,12,13,2,3,6,7,10,11,14,15};
    for (unsigned n=0;n<16;++n) check(gs.local_word(offsets[n])==n+1,"PSMCT32 column layout differs");
    setup(0,0,63,31,2,2);
    gs.upload({0x2222222211111111ULL,0x4444444433333333ULL});
    check(gs.local_word(2047)==0x11111111 && gs.local_word(2738)==0x22222222 &&
          gs.local_word(5453)==0x33333333 && gs.local_word(6144)==0x44444444,"Page/row boundary upload differs");
    setup(0,0x3fff,0,0,4,1);
    gs.upload({~0ULL,~0ULL});
    setup(1,0x3fff,0,0,6,1);
    // Continuous 24-bit bytes span two IMAGE packets; high destination bytes survive.
    gif.consume({(2ULL<<58)|1,0},gs);
    gif.consume({0x0807060504030201ULL,0x100f0e0d0c0b0a09ULL},gs);
    gif.consume({(3ULL<<58)|1,0},gs); gif.consume({0x1211,0},gs);
    check(gs.local_word(0xfffc0)==0xff030201 && gs.local_word(0xfffc1)==0xff060504 &&
          gs.local_word(0xfffc4)==0xff090807 && gs.local_word(0xfffc9)==0x121110,
          "PSMCT24 stream packing, packet split or alpha preservation differs");
    setup(0,0x3fff,8,0,1,1); gs.upload({0xaabbccdd,0});
    check(gs.local_word(0)==0xaabbccdd,"Local-memory address did not wrap at four MiB");
    gs.write(0x12001000,8,0x200);
    check(gs.local_word(0)==0xaabbccdd,"GS reset erased local memory");
    rejects([&]{gs.upload({});});
    setup(0,0,0,0,4,1);
    nt::runtime::RamBus ram;
    nt::hardware::GraphicsDma dma;
    nt::hardware::Vif vif;
    gif.write(0x10003000,4,1);
    ram.store_quad(0x1000,{(2ULL<<58)|1,0});
    ram.store_quad(0x1010,{0x2222222211111111ULL,0x4444444433333333ULL});
    dma.write(0x1000a010,4,0x1000); dma.write(0x1000a020,4,2); dma.write(0x1000a000,4,0x101);
    check(dma.step(ram,vif,gif,gs,true)==0 && dma.read(0x1000a020,4)==1,"IMAGE tag completed DMA before pixels");
    check(dma.step(ram,vif,gif,gs,true)==4 && gs.local_word(0)==0x11111111 &&
          gs.local_word(5)==0x44444444,"IMAGE DMA completion or pixel delivery differs");
}
int main() {
    try {
        gs_initialization();
        gs_scanout();
        vif_initialization();
        gif_initialization();
        vif_dma_transport();
        gif_packets();
        gs_image_uploads();
        nt::hardware::SystemBus bus;
        bus.store(0x80001000,4,0x11223344);
        check(bus.load(0xa0001000,4) == 0x11223344 && bus.load(0x1000,4) == 0x11223344,"RAM aliases disagree");
        rejects([&] { bus.load(0xc0001000,4); });
        for (unsigned i = 0; i < 4; ++i) {
            const auto base = 0x10000000 + i * 0x800;
            check(bus.load(base+16,4) == 0 && bus.load(base+32,4) == 65535,"Timer reset state differs");
            bus.store(base,4,0x12345); check(bus.load(base,4) == 0x2345,"Timer count width differs");
            bus.store(base+32,4,0x23456); check(bus.load(base+32,4) == 0x3456,"Timer target width differs");
            rejects([&] { bus.store(base+16,4,0x84); });
            check(bus.load(base+16,4) == 0,"Rejected timer activation mutated state");
        }
        rejects([&] { bus.load(0x10001830,4); });
        rejects([&] { bus.load(0x10000010,8); });
        rejects([&] { bus.load(0x10003700,4); });
        bus.store(0x1000f010,4,0x1000); check(bus.load(0x1000f010,4) == 0x1000,"INTC mask toggle failed");
        bus.store(0x1000f010,4,0x1000); check(bus.load(0x1000f010,4) == 0,"INTC mask clear toggle failed");
        nt::kernel::Kernel kernel;
        nt::runtime::State s;
        s.gpr[3].lo = 0x74; s.gpr[4].lo = 0x12c; s.gpr[5].lo = 0x80002000;
        check(kernel.service(s,bus) && kernel.interrupt_handler(12) == 0x2000 && bus.load(0x1000f010,4) == 0x1000,"Timer interrupt alias registration failed");
        s.gpr[3].lo = 0x64;
        for (unsigned op=0; op<4; ++op) { s.gpr[4].lo=op; check(kernel.service(s,bus),"FlushCache rejected"); }
        rejects([&] { bus.synchronize_cache(4); });
        bus.store(0x10001800,4,0); bus.store(0x10001820,4,3);
        bus.store(0x10001810,4,0x1c0); // Count + target IRQ + zero return, bus clock.
        bus.advance_cycles(5); check(bus.load(0x10001800,4) == 2 && !bus.interrupt_pending(),"Timer divider/phase failed");
        bus.advance_cycles(1); check(bus.load(0x10001800,4) == 0 && bus.interrupt_pending(),"Target interrupt or zero return failed");
        check((bus.load(0x10001810,4)&0x400) != 0,"Target flag missing");
        bus.store(0x1000f000,4,0x1000); bus.store(0x10001810,4,0x400);
        check(!bus.interrupt_pending() && bus.load(0x10001810,4) == 0,"Interrupt/status W1C failed");
        bus.store(0x10001800,4,65535); bus.store(0x10001810,4,0x282);
        bus.advance_cycles(512); check(bus.load(0x10001800,4) == 0 && (bus.load(0x10001810,4)&0x800),"Overflow/divided timer failed");
        std::cout << "GS initialization, RAM aliases, timer registers, INTC and cache tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
