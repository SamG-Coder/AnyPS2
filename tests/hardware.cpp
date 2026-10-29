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
int main() {
    try {
        gs_initialization();
        gs_scanout();
        vif_initialization();
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
        rejects([&] { bus.load(0x10003000,4); });
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
