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
int main() {
    try {
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
        std::cout << "RAM aliases, timer registers, INTC and cache tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
