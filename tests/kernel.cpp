#include <nt/kernel.hpp>
#include <iostream>

using namespace nt::runtime;
namespace {
void check(bool v, const char* m) { if (!v) throw std::runtime_error(m); }
State setup() {
    State s;
    s.gpr[3].lo = 0x3c;
    s.gpr[4].lo = 0x1234;
    s.gpr[5].lo = 0x1ff8000;
    s.gpr[6].lo = 0x8000;
    s.gpr[7].lo = 0x4000;
    s.gpr[8].lo = 0x2000;
    return s;
}
Result boundary(State& s, Bus&, std::uint64_t) { return {Stop::syscall,s.pc,12}; }
}
int main() {
    try {
        RamBus memory;
        nt::kernel::Kernel kernel;
        auto s = setup();
        nt::kernel::Kernel::initialize_cpu(s);
        check(s.cop0_status == 0x10c01 && s.gpr[3].lo == 0x3c,"BIOS-free CPU handoff changed arguments or interrupt masks");
        memory.store(0x4000,8,0xffffffffffffffffULL);
        check(kernel.service(s,memory), "SetupThread rejected");
        check(s.gpr[2].lo == 0x1fffd60 && memory.load(0x4000,8) == 0, "Stack or argument initialization failed");
        check(kernel.main_thread().id == 1 && kernel.main_thread().stack_base == 0x1ff8000, "Thread metadata incorrect");
        check(!kernel.service(s,memory), "Unimplemented thread reinitialization accepted");
        s.gpr[3].lo = 0x3d; s.gpr[4].lo = 0x10519ac; s.gpr[5].lo = 0xffffffff;
        check(kernel.service(s,memory) && s.gpr[2].lo == 0x1ff8000, "Default heap boundary failed");
        s.gpr[3].lo = 0x3e; s.gpr[2].lo = 0;
        check(kernel.service(s,memory) && s.gpr[2].lo == 0x1ff8000, "EndOfHeap failed");
        s.gpr[3].lo = 0x3d; s.gpr[4].lo = 0x10000; s.gpr[5].lo = 0x2000;
        check(kernel.service(s,memory) && s.gpr[2].lo == 0x12000, "Explicit heap size failed");
        s.gpr[3].lo = 0x999; s.gpr[2].lo = 123;
        check(!kernel.service(s,memory) && s.gpr[2].lo == 123, "Unknown syscall manufactured a result");
        nt::kernel::Kernel fresh;
        s = setup(); s.gpr[5].lo = 0xffffffff;
        check(fresh.service(s,memory) && s.gpr[2].lo == 0x1ffed60, "Default stack failed");
        nt::kernel::Kernel invalid;
        s = setup(); s.gpr[5].lo = 0xffffff00;
        bool rejected = false;
        try { invalid.service(s,memory); } catch (const MemoryFault&) { rejected = true; }
        check(rejected && invalid.main_thread().id == 0, "Overflowing stack accepted");
        nt::kernel::Kernel driver;
        s = setup(); s.pc = 0x1000; s.delay_slot = true; s.pending_target = 0x2000;
        const auto stop = nt::kernel::run(boundary,s,memory,driver,1);
        check(stop.stop.reason == Stop::budget && stop.serviced_calls == 1 && s.pc == 0x2000 && !s.delay_slot && s.executed == 1, "HLE budget/delay-slot return failed");
        s.gpr[3].lo = 0x999;
        const auto unsupported = nt::kernel::run(boundary,s,memory,driver,10);
        check(unsupported.stop.reason == Stop::syscall && unsupported.serviced_calls == 0 && s.pc == 0x2000, "Unhandled syscall was skipped");
        memory.store(0x5004,4,1); memory.store(0x5008,4,1);
        s.gpr[3].lo = 0x40; s.gpr[4].lo = 0x5000;
        check(kernel.service(s,memory) && s.gpr[2].lo == 1, "Semaphore creation failed");
        s.gpr[3].lo = 0x44; s.gpr[4].lo = 1;
        check(kernel.service(s,memory), "Available semaphore wait failed");
        check(kernel.service(s,memory) && kernel.waiting(), "Empty wait did not suspend the last runnable thread");
        kernel.schedule(s);
        check(kernel.waiting(), "Scheduling falsely resumed an idle semaphore waiter");
        s.gpr[3].lo = 0x45;
        check(kernel.service(s,memory) && s.gpr[2].lo == ~std::uint64_t(0), "Empty semaphore poll failed");
        s.gpr[3].lo = static_cast<std::uint64_t>(-0x43LL);
        check(kernel.service(s,memory) && s.gpr[2].lo == 1 && !kernel.waiting(), "Interrupt signal did not wake the waiter");
        check(kernel.service(s,memory), "Signal failed to restore a free permit after waking the waiter");
        check(!kernel.service(s,memory), "Semaphore overflow silently accepted");
        s.gpr[3].lo = 0x47; s.gpr[5].lo = 0x5100;
        check(kernel.service(s,memory) && memory.load(0x5100,4) == 1 && memory.load(0x5104,4) == 1, "Semaphore status failed");
        s.gpr[3].lo = 0x41;
        check(kernel.service(s,memory) && s.gpr[2].lo == 1, "Semaphore deletion failed");
        check(kernel.service(s,memory) && s.gpr[2].lo == ~std::uint64_t(0), "Deleted semaphore remained valid");
        std::cout << "BIOS-free startup kernel validation passed\n";
        s.gpr[3].lo = 0x4b; s.gpr[4].lo = 0x6000;
        check(kernel.service(s,memory) && memory.load(0x6000,4) == 0x12010, "Default OSD configuration failed");
        memory.store(0x6000,4,0x12345678);
        s.gpr[3].lo = 0x4a;
        check(kernel.service(s,memory), "OSD configuration update failed");
        memory.store(0x6000,4,0);
        s.gpr[3].lo = 0x4b;
        check(kernel.service(s,memory) && memory.load(0x6000,4) == 0x12345678, "OSD configuration round trip failed");
        s.gpr[4].lo = 0x6001;
        rejected = false;
        try { kernel.service(s,memory); } catch (const MemoryFault&) { rejected = true; }
        check(rejected, "Unaligned OSD configuration write accepted");
        s.set(3,0x6f); s.set(4,0x6001); s.set(5,1); s.set(6,1);
        check(kernel.service(s,memory) && memory.load(0x6001,1)==0,"Default OSD2 byte read failed");
        memory.store(0x6001,1,0x30); s.set(3,0x6e);
        check(kernel.service(s,memory),"OSD2 update failed");
        memory.store(0x6001,1,0); s.set(3,0x6f);
        check(kernel.service(s,memory) && memory.load(0x6001,1)==0x30,"OSD2 round trip failed");
        s.set(6,4); check(!kernel.service(s,memory),"Unknown OSD2 extent accepted");
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
