#include <nt/kernel.hpp>
#include <iostream>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void test() {
    nt::runtime::RamBus memory;
    nt::kernel::Kernel kernel;
    nt::runtime::State state;
    const auto call = [&](unsigned number, unsigned a0 = 0, unsigned a1 = 0) {
        state.set(3,number); state.set(4,a0); state.set(5,a1);
        check(kernel.service(state,memory), "Expected thread service was unsupported");
        return state.gpr[2].lo;
    };
    state.set(6,0x1000); state.set(7,0x3000); state.set(8,0x2000);
    call(0x3c,0x1230,0x1fff000);
    check(call(0x2f) == 1, "Main thread ID incorrect");
    const std::array<unsigned,9> descriptor{0,0x2000,0x8000,0x1000,0x123456,42,99,7,8};
    for (unsigned i = 0; i < descriptor.size(); ++i) memory.store(0x4000 + i * 4,4,descriptor[i]);
    check(call(0x20,0x80004000) == 2, "CreateThread or descriptor alias failed");
    check(call(0x20,0x4000) == 3, "Thread IDs collided");
    check(call(0x30,2,0x5000) == 0x10, "New thread was not dormant");
    for (unsigned i = 1; i < descriptor.size(); ++i)
        check(memory.load(0x5000 + i * 4,4) == (i == 6 ? 42 : descriptor[i]), "Thread descriptor field lost");
    check(memory.load(0x5024,4) == 0 && memory.load(0x502c,4) == 0, "Dormant thread has wait state");
    check(call(0x31,0,0) == 1, "Self status query failed");
    check(call(0x29,0,50) == 0 && call(0x29,0,128) == ~std::uint64_t(0), "Priority change validation failed");
    check(call(0x22,3,0x1234) == 3, "StartThread failed");
    check(call(0x22,3,0) == ~std::uint64_t(0) && call(0x21,3) == ~std::uint64_t(0), "Started thread restarted or deleted");
    check(call(0x30,3,0) == 2, "Started thread not ready before scheduling");
    check(call(0x21,1) == ~std::uint64_t(0), "Deleted current thread");
    check(call(0x21,2) == 2 && call(0x30,2) == 0, "DeleteThread/status failed");
    check(call(0x20,0x4000) == 2, "Deleted thread ID not reusable");
    memory.store(0x4014,4,128);
    check(call(0x20,0x4000) == ~std::uint64_t(0), "Invalid priority accepted");
    memory.store(0x4014,4,42);
    memory.store(0x400c,4,0xfffffff0U);
    bool fault = false;
    try { call(0x20,0x4000); } catch (const nt::runtime::MemoryFault&) { fault = true; }
    check(fault, "Wrapping stack accepted");
    memory.store(0x400c,4,0x1000);
    check(call(0x20,0x4000) == 4, "Failed create consumed a thread ID");
    memory.store(0x4014,4,0);
    fault = false;
    try { call(0x20,0x1ffffe0); } catch (const nt::runtime::MemoryFault&) { fault = true; }
    check(fault, "Truncated descriptor accepted");
    for (unsigned id = 5; id < 256; ++id) check(call(0x20,0x4000) == id, "Thread allocation failed");
    check(call(0x20,0x4000) == ~std::uint64_t(0), "Thread limit exceeded");
    check(call(0x30,255,0x5000) == 0x10 && memory.load(0x5014,4) == 0,
          "Kernel priority zero was not preserved");
    check(call(0x22,4) == 4 && call(0x22,2) == 2, "Ready queue setup failed");
    kernel.schedule(state);
    check(call(0x2f) == 3, "Ready queue did not select the oldest equal-priority thread");
    check(call(0x29,3,100) == 42, "Priority change did not return the previous value");
    kernel.schedule(state);
    check(call(0x2f) == 4, "Ready queue used thread ID instead of insertion order");
    bool status_fault = false;
    try { call(0x30,2,0x1fffff0); } catch (const nt::runtime::MemoryFault&) { status_fault = true; }
    check(status_fault && call(0x30,2) == 2, "Invalid status buffer corrupted a ready thread");
    check(call(0x33,0)==~std::uint64_t(0) && call(0x33,4)==~std::uint64_t(0),"Wakeup accepted invalid/self target");
    check(call(0x33,2)==2 && call(0x33,2)==2,"Ready thread did not queue wakeups");
    call(0x30,2,0x5000);
    check(memory.load(0x502c,4)==2,"Wakeup count missing in thread status");
    check(call(0x35,2)==2 && call(0x36,2)==0,"CancelWakeup did not return and clear count");
    check(call(0x33,2)==2,"Queued wakeup setup failed");
    call(0x29,2,0); kernel.schedule(state);
    check(call(0x2f)==2 && call(0x32)==2 && !kernel.waiting(),"Sleep failed to consume queued wakeup");
    check(call(0x32)==2 && kernel.waiting(),"Sleep failed to block");
    kernel.schedule(state);
    check(call(0x2f)==4 && call(0x34,2)==2,"Sleeping thread did not wake");
    kernel.schedule(state);
    check(call(0x2f)==2 && !kernel.waiting(),"Wakeup failed to restore higher-priority thread");
}
}
int main() {
    try { test(); std::cout << "BIOS-free thread lifecycle tests passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
