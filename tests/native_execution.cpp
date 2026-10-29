#include "native_fixture.hpp"
#include <nt/runtime.hpp>
#include <nt/analysis.hpp>
#include <nt/kernel.hpp>
#include <nt/system_bus.hpp>
#include <iostream>
#include <limits>

using namespace nt::runtime;
Result nt_generated_run(State&, Bus&, std::uint64_t);
namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
State at(std::uint32_t pc) { State s; s.pc = pc; return s; }
void sif_interrupt_delivery() {
    nt::hardware::SystemBus bus;
    nt::kernel::Kernel kernel;
    for (const auto& [pc,word] : fixture::words()) bus.store(pc,4,word);
    State registration;
    unsigned registered = 0;
    for (const auto address : {0x9000U,0x9100U,0x9000U}) {
        registration.gpr[3].lo = 0x12; registration.gpr[4].lo = 5;
        registration.gpr[5].lo = address; registration.gpr[6].lo = 0xffffffff;
        registration.gpr[7].lo = registered++ == 2 ? 0x20100700 : 0x20100600;
        registration.gpr[28].lo = 0x12345678;
        check(kernel.service(registration,bus),"DMA handler registration failed");
    }
    // Give the final handler a different argument so a negative predecessor must skip it.
    // Each handler writes its channel, GP and a delay-slot marker into guest RAM.
    auto state = at(0x9200);
    kernel.initialize_cpu(state);
    for (unsigned i = 1; i < 32; ++i) state.gpr[i] = {i * 17ULL,i * 31ULL};
    state.gpr[29].lo = 0x1fff000;
    state.fpr[3] = 0x3f800000; state.hi = 123; state.lo = 456;
    const auto original = state;
    std::vector<std::uint8_t> payload(32,0x5a);
    bus.sif().iop_install(0x1000,payload); bus.sif().iop_send(0x1000,0x104000,2);
    bus.store(0x1000e000,4,1); bus.store(0x1000e010,4,1U << 21);
    bus.store(0x1000c000,4,0x104); bus.advance_cycles(20);
    check(bus.interrupt_lines() == 0x800,"SIF0 did not raise DMA completion");
    auto result = nt::kernel::run(nt_generated_run,state,bus,kernel,2);
    check(result.stop.reason == Stop::budget && kernel.interrupt_active() && (state.cop0_status & 2),
          "Interrupt entry did not survive budget suspension");
    check(bus.load(0x1000e010,4) == (1U << 21),"Interrupt acknowledgement changed mask or retained completion");
    result = nt::kernel::run(nt_generated_run,state,bus,kernel,100);
    check(result.stop.reason == Stop::unsupported && state.pc == 0x9204 && !kernel.interrupt_active() && kernel.completed_interrupts() == 1,
          "DMA callback did not return to interrupted mainline");
    check(state.executed == 11 && bus.load(0x100600,4) == 5 && bus.load(0x30100604,4) == 0x12345678 &&
          bus.load(0x100608,4) == 99 && bus.load(0x10060c,4) == 0xffffffff && bus.load(0x100700,4) == 0,
          "DMA callback ordering, arguments, negative return or delay slot failed");
    for (unsigned i = 0; i < 32; ++i)
        check(state.gpr[i].lo == original.gpr[i].lo && state.gpr[i].hi == original.gpr[i].hi,
              "Interrupt lost a 128-bit guest register");
    check(state.fpr == original.fpr && state.hi == original.hi && state.lo == original.lo &&
          state.cop0_status == original.cop0_status && state.cop0_epc == original.cop0_epc,
          "Interrupt lost floating-point, multiply or exception context");
    check(bus.load(0x20104000,8) == 0x5a5a5a5a5a5a5a5aULL,"SIF0 payload lost through uncached RAM mapping");
    const std::array<SifTransfer,1> reply{{{0x30104000,0x1800,32,0x44}}};
    const auto job = bus.submit_sif(reply);
    bus.advance_cycles(40);
    check(job && bus.sif_status(job) == -1 && bus.sif().iop_memory()[0x1800] == 0x5a,
          "SIF reply from uncached guest RAM failed");
    for (auto address : {0x200ffffcU,0x22000000U,0x300ffffcU,0x32000000U}) {
        bool rejected = false;
        try { bus.load(address,4); } catch (const MemoryFault&) { rejected = true; }
        check(rejected,"Default RAM aliases admitted an unmapped address");
    }
}
void dma_interrupt_mask() {
    nt::hardware::SystemBus bus;
    bus.store(0x1000,4,fixture::words().at(0x1000));
    bus.store(0x1000e000,4,1); bus.store(0x1000e010,4,1U << 21);
    bus.store(0x1000c000,4,0x100); bus.advance_cycles(1);
    auto state = at(0x1000); state.cop0_status = 0x10401;
    check(nt_generated_run(state,bus,1).reason == Stop::budget,"DMA incorrectly used INTC CPU mask");
    state = at(0x1000); state.cop0_status = 0x10801;
    check(nt_generated_run(state,bus,1).reason == Stop::interrupt_pending,"DMA CPU interrupt mask ignored");
    state = at(0x1000); state.cop0_status = 0x10803;
    check(nt_generated_run(state,bus,1).reason == Stop::budget,"DMA interrupted exception level");
}
void test(RamBus& bus) {
    auto s = at(0x1000); s.gpr[1].lo = 0x7fffffff; s.gpr[2].hi = 0x123456789abcdef0;
    check(nt_generated_run(s,bus,1).reason == Stop::budget, "ADDIU execution failed");
    check(s.gpr[2].lo == 0xffffffff80000000ULL && s.gpr[2].hi == 0x123456789abcdef0, "32-bit wrap or upper-lane preservation failed");
    s = at(0x1100); nt_generated_run(s,bus,1);
    check(s.gpr[2].lo == 0xffffffff80000000ULL, "LUI sign extension failed");
    s = at(0x1200); s.gpr[1].lo = 0x100000000ULL; nt_generated_run(s,bus,1);
    check(s.gpr[2].lo == 1, "SLTIU immediate extension failed");
    s = at(0x1300); s.gpr[1] = {0xfffffffffffffffe, 0x8000000000000001}; s.gpr[2] = {0x0000000100000003, 0x8000000000000002};
    nt_generated_run(s,bus,1);
    check(s.gpr[3].lo == 0xffffffffffffffffULL && s.gpr[3].hi == 0xffffffff00000003ULL, "PADDUW lane saturation failed");
    s = at(0x1400); s.gpr[1].lo = s.gpr[2].lo = 9;
    nt_generated_run(s,bus,1);
    check(s.pc == 0x1404 && s.delay_slot && s.pending_target == 0x1410, "Branch delay state failed");
    nt_generated_run(s,bus,1);
    check(s.pc == 0x1410 && !s.delay_slot && s.gpr[3].lo == 1, "Delay slot resume failed");
    check(nt_generated_run(s,bus,1).reason == Stop::syscall && s.executed == 2, "Syscall boundary mutated execution count");
    s = at(0x1400); s.gpr[1].lo = 1;
    nt_generated_run(s,bus,2);
    check(s.pc == 0x1408 && s.gpr[3].lo == 1, "Non-taken branch omitted delay slot");
    s = at(0x1500); s.gpr[1].lo = 1; s.gpr[4].lo = 0x10000000;
    const auto likely = nt_generated_run(s,bus,2);
    check(likely.reason == Stop::syscall && s.pc == 0x1508 && s.executed == 1, "Branch-likely annul failed");
    s = at(0x1600); s.gpr[5].lo = 0x1700;
    nt_generated_run(s,bus,2);
    check(s.pc == 0x1700 && s.gpr[5].lo == 0x1608 && s.gpr[3].lo == 1, "JALR alias or delay slot failed");
    bus.store(0x8000,4,0x80000001);
    s = at(0x1800); s.gpr[1].lo = 0x8000; nt_generated_run(s,bus,1);
    check(s.gpr[2].lo == 0xffffffff80000001ULL, "LW sign extension failed");
    s = at(0x1900); s.gpr[1].lo = 0x8004; s.gpr[2].lo = 0xdeadbeef01234567ULL; nt_generated_run(s,bus,1);
    check(bus.load(0x8004,4) == 0x01234567, "SW failed");
    s = at(0x1b00); s.gpr[1].lo = 0x8107; s.gpr[2] = {0x123456789abcdef0,0xfedcba9876543210}; nt_generated_run(s,bus,1);
    s = at(0x1a00); s.gpr[1].lo = 0x810f; nt_generated_run(s,bus,1);
    check(s.gpr[2].lo == 0x123456789abcdef0 && s.gpr[2].hi == 0xfedcba9876543210, "LQ/SQ alignment or lanes failed");
    s = at(0x1c00); s.gpr[0] = {123,456}; nt_generated_run(s,bus,1);
    check(s.gpr[0].lo == 0 && s.gpr[0].hi == 0, "Zero register modified");
    s = at(0x1d00); check(nt_generated_run(s,bus,10).reason == Stop::unsupported && s.executed == 0, "Unsupported code did not stop");
    s = at(0x1e00); check(nt_generated_run(s,bus,2).reason == Stop::branch_in_delay && s.pc == 0x1e04, "Nested branch did not stop");
    s = at(0x1f00); s.gpr[4].lo = 0x10000000;
    check(nt_generated_run(s,bus,2).reason == Stop::memory_fault && s.pc == 0x1f04 && s.delay_slot && s.branch_pc == 0x1f00, "Delay-slot fault context lost");
    s = at(0x2000); s.gpr[1].lo = 0x80000000; nt_generated_run(s,bus,1);
    check(s.gpr[2].lo == 0xfffffffff8000000ULL, "SRA failed");
    s = at(0x2100); s.gpr[1].lo = 0x1234567887654321; nt_generated_run(s,bus,1);
    check(s.sa == 0x87654321, "MTSA width failed");
    s = at(0x2200); s.gpr[1].lo = 0x8000000000000000; s.gpr[2].lo = 1; nt_generated_run(s,bus,1);
    check(s.gpr[3].lo == 0x8000000000000001, "64-bit OR failed");
    bus.store(0x8200,2,0x8080);
    for (auto pc : {0x2300U,0x2400U}) {
        s = at(pc); s.gpr[1].lo = 0x8200; nt_generated_run(s,bus,1);
        check(s.gpr[2].lo == (pc == 0x2300 ? 0xffffffffffffff80ULL : 0xffffffffffff8080ULL), "LB/LH sign extension failed");
    }
    s = at(0x2600); s.gpr[1].lo = 0x8300; s.gpr[2].lo = 0xfedcba9876543210; nt_generated_run(s,bus,1);
    s = at(0x2500); s.gpr[1].lo = 0x8300; nt_generated_run(s,bus,1);
    check(s.gpr[2].lo == 0xfedcba9876543210, "LD/SD failed");
    s = at(0x1800); s.gpr[1].lo = 0x8001; s.gpr[2].lo = 99;
    check(nt_generated_run(s,bus,1).reason == Stop::memory_fault && s.gpr[2].lo == 99 && s.pc == 0x1800, "Unaligned load mutated state");
    s = at(0x2700); s.gpr[2].lo = 0x1234567880000001; nt_generated_run(s,bus,1);
    check(s.fpr[3] == 0x80000001, "MTC1 did not preserve raw bits");
    s.pc = 0x2800; nt_generated_run(s,bus,1);
    check(s.gpr[2].lo == 0xffffffff80000001ULL, "MFC1 sign extension failed");
    s = at(0x2900); s.fcr31 = 0xffffffff; nt_generated_run(s,bus,1);
    check(s.accumulator == 0 && s.fcr31 == 0xffff3fff, "ADDA.S zero specialization flags failed");
    s = at(0x2900); s.fpr[1] = 0x3f800000; s.accumulator = 99;
    check(nt_generated_run(s,bus,1).reason == Stop::unsupported && s.accumulator == 99 && s.executed == 0, "Nonzero FPU input incorrectly accepted");
    s = at(0x2a00); s.gpr[2].lo = 0x80808080; nt_generated_run(s,bus,1);
    s.pc = 0x2b00; nt_generated_run(s,bus,1);
    check(s.gpr[2].lo == 0xffffffff80808080ULL, "FPU control transfer failed");
    s = at(0x1000); bus.store(0x1000,4,0);
    check(nt_generated_run(s,bus,1).reason == Stop::code_changed && s.executed == 0, "Code invalidation was ignored");
    s = at(0x3000); check(nt_generated_run(s,bus,1).reason == Stop::unmapped_code, "Unknown destination did not stop");
}
void handlers(RamBus& bus) {
    nt::kernel::Kernel kernel;
    auto s = at(0);
    const auto install = [&](unsigned number, unsigned address) {
        s.gpr[3].lo = 0x74; s.gpr[4].lo = number; s.gpr[5].lo = address;
        check(kernel.service(s,bus) && s.gpr[2].lo == 0, "SetSyscall failed");
    };
    install(0x90,0x80006000);
    check(bus.load(0x80010000 + 0x90 * 4,4) == 0x80006000, "Guest syscall table missing");
    s = at(0x6100); s.gpr[3].lo = 0x90; s.gpr[4].lo = 10; s.gpr[29].lo = 0x9000; s.gpr[31].lo = 0xabcdef;
    auto result = nt::kernel::run(nt_generated_run,s,bus,kernel,2);
    check(result.stop.reason == Stop::budget && kernel.handler_depth() == 1 && s.pc == 0x6004, "Handler budget resume state lost");
    result = nt::kernel::run(nt_generated_run,s,bus,kernel,20);
    check(result.stop.reason == Stop::unsupported && s.pc == 0x6104 && s.gpr[2].lo == 15 && s.gpr[7].lo == 42, "Guest handler execution failed");
    check(s.gpr[29].lo == 0x9000 && s.gpr[31].lo == 0xabcdef && kernel.handler_depth() == 0, "Guest handler return frame corrupted");
    install(0x91,0x6000); install(0x90,0x6200);
    s = at(0x6100); s.gpr[3].lo = 0x90; s.gpr[4].lo = 20; s.gpr[29].lo = 0x9000; s.gpr[31].lo = 0x1234;
    result = nt::kernel::run(nt_generated_run,s,bus,kernel,30);
    check(result.stop.reason == Stop::unsupported && s.gpr[2].lo == 25 && s.gpr[31].lo == 0x1234 && s.gpr[29].lo == 0x9000 && kernel.handler_depth() == 0, "Nested guest handler return failed");
    install(0x90,0x6000);
    s = at(0x6300); s.gpr[3].lo = static_cast<std::uint64_t>(-0x90LL); s.gpr[29].lo = 0x9000;
    result = nt::kernel::run(nt_generated_run,s,bus,kernel,20);
    check(result.stop.reason == Stop::unsupported && s.pc == 0x6320 && !s.delay_slot, "Syscall delay-slot continuation failed");
    bus.store(nt::kernel::Kernel::syscall_table + 0x90 * 4,4,0x6200);
    check(kernel.handler(0x90,bus) == 0x6200, "Guest table patch ignored");
    install(0x90,0);
    check(kernel.handler(0x90,bus) == 0, "Handler removal failed");
    s.gpr[4].lo = 256; s.gpr[5].lo = 0x6000;
    check(!kernel.service(s,bus), "Out-of-range SetSyscall accepted");
}
void thread_switches(RamBus& bus) {
    nt::kernel::Kernel kernel;
    auto s = at(0);
    const auto service = [&](unsigned call, unsigned a0, unsigned a1) {
        s.set(3,call); s.set(4,a0); s.set(5,a1);
        check(kernel.service(s,bus), "Thread fixture service failed");
    };
    s.set(6,0x1000); s.set(7,0x3000); s.set(8,0x7004);
    service(0x3c,0x1234,0x1fff000);
    service(0x29,1,10);
    const std::array<unsigned,9> descriptor{0,0x7000,0x8000,0x1000,0xabcd,5,0,0,0};
    for (unsigned i = 0; i < descriptor.size(); ++i) bus.store(0x4000 + i * 4,4,descriptor[i]);
    service(0x20,0x4000,0);
    check(s.gpr[2].lo == 2, "Child thread allocation failed");
    s.pc = 0x7100; s.set(3,0x22); s.set(4,2); s.set(5,0xfeed);
    s.gpr[16] = {123,456}; s.fpr[6] = 0x87654321; s.hi1 = 987;
    s.set(29,0x1fffd60); s.cop0_status = 0x10001;
    auto result = nt::kernel::run(nt_generated_run,s,bus,kernel,2);
    check(result.stop.reason == Stop::budget && s.pc == 0x7004 && s.executed == 2,
          "StartThread did not switch at the syscall boundary with exact budget");
    check(s.gpr[4].lo == 0xfeed && s.gpr[28].lo == 0xabcd && s.gpr[29].lo == 0x8d60 &&
          s.gpr[31].lo == 0x7004 && s.gpr[16].lo == 42 && s.gpr[16].hi == 0,
          "Child argument, GP, SP, root, or fresh registers incorrect");
    check(s.cop0_status == 0x10001 && s.fpr[6] == 0, "CPU status or child FPU initialization failed");
    result = nt::kernel::run(nt_generated_run,s,bus,kernel,10);
    check(result.stop.reason == Stop::unsupported && s.pc == 0x7104 && s.executed == 4,
          "ExitThread did not resume caller after StartThread");
    check(s.gpr[16].lo == 123 && s.gpr[16].hi == 456 && s.fpr[6] == 0x87654321 && s.hi1 == 987 &&
          s.gpr[29].lo == 0x1fffd60 && s.gpr[2].lo == 2,
          "Thread switch lost full register context or syscall result");
    service(0x30,2,0); check(s.gpr[2].lo == 0x10, "Exited thread not dormant");
    s.pc = 0x7100; s.set(3,0x22); s.set(4,2); s.set(5,0xbeef);
    result = nt::kernel::run(nt_generated_run,s,bus,kernel,1);
    check(s.pc == 0x7000 && s.gpr[16].lo == 0 && s.gpr[4].lo == 0xbeef && s.executed == 5,
          "Restart did not reset the child context");
    nt::kernel::run(nt_generated_run,s,bus,kernel,10);
    s.pc = 0x7800; s.set(3,0x22); s.set(4,2); s.set(5,0);
    result = nt::kernel::run(nt_generated_run,s,bus,kernel,20);
    check(result.stop.reason == Stop::unsupported && s.pc == 0x7820 && !s.delay_slot,
          "Thread switch lost a syscall's branch-delay continuation");
    service(0x29,1,5);
    s.pc = 0x7100; s.set(3,0x22); s.set(4,2);
    result = nt::kernel::run(nt_generated_run,s,bus,kernel,10);
    check(result.stop.reason == Stop::unsupported && s.pc == 0x7104,
          "Equal-priority StartThread unexpectedly preempted the caller");
    service(0x29,1,6); kernel.schedule(s);
    check(s.pc == 0x7000, "Priority change failed to select the higher-priority ready thread");
    nt::kernel::run(nt_generated_run,s,bus,kernel,10);
    s.set(3,0x23);
    check(!kernel.service(s,bus), "ExitThread invented an idle successor");
}
void semaphore_switches(RamBus& bus) {
    nt::kernel::Kernel kernel;
    auto s = at(0);
    const auto call = [&](unsigned number, unsigned a0, unsigned a1 = 0) {
        s.set(3,number); s.set(4,a0); s.set(5,a1);
        check(kernel.service(s,bus),"Semaphore scheduling setup failed");
    };
    s.set(6,0x1000); s.set(7,0x3000); s.set(8,0x8510);
    call(0x3c,0,0x1fff000); call(0x29,1,10);
    for (unsigned offset = 0; offset < 24; offset += 4) bus.store(0x5000+offset,4,0);
    bus.store(0x5004,4,1); call(0x40,0x5000);
    const std::array<unsigned,9> descriptor{0,0x8500,0x9000,0x1000,0,5,0,0,0};
    for (unsigned i = 0; i < descriptor.size(); ++i) bus.store(0x4000+i*4,4,descriptor[i]);
    call(0x20,0x4000);
    s.pc = 0x8600; s.set(3,0x22); s.set(4,2); s.set(5,0); s.set(16,123);
    auto result = nt::kernel::run(nt_generated_run,s,bus,kernel,4);
    check(result.stop.reason == Stop::budget && s.pc == 0x8604,"Empty semaphore did not block child and resume parent");
    const auto parent = s;
    call(0x30,2,0x5100);
    check(bus.load(0x5100,4) == 4 && bus.load(0x5124,4) == 2 && bus.load(0x5128,4) == 1,"Waiting thread status incorrect");
    call(0x47,1,0x5200); check(bus.load(0x520c,4) == 1,"Semaphore waiter count missing");
    s.set(3,0x41); s.set(4,1); check(!kernel.service(s,bus),"Deleted semaphore with a blocked waiter");
    s = parent;
    result = nt::kernel::run(nt_generated_run,s,bus,kernel,30);
    check(result.stop.reason == Stop::unsupported && s.pc == 0x8610 && s.gpr[16].lo == 123,
          "Semaphore signal failed to wake child and restore parent");
    call(0x47,1,0x5200);
    check(bus.load(0x5200,4) == 0 && bus.load(0x520c,4) == 0,"Signal both woke waiter and incremented semaphore");
}
void multiply_divide(RamBus& bus) {
    struct DivisionCase { unsigned a, b, quotient, remainder; bool signed_operation; };
    for (const auto& c : {
        DivisionCase{17,5,3,2,true}, {0xffffffefU,5,0xfffffffdU,0xfffffffeU,true},
        {0x80000000U,0xffffffffU,0x80000000U,0,true}, {0,0,0xffffffffU,0,true},
        {7,0,0xffffffffU,7,true}, {0x80000000U,0,1,0x80000000U,true},
        {0xffffffffU,1,0xffffffffU,0,false}, {0xffffffffU,0,0xffffffffU,0xffffffffU,false},
        {0xfffffffeU,0xffffffffU,0,0xfffffffeU,false}}) {
        auto state = at(c.signed_operation ? 0x7c00 : 0x7d00);
        state.set(1,c.a); state.set(2,c.b); state.hi1 = 123; state.lo1 = 456;
        nt_generated_run(state,bus,1);
        check(state.lo == sx32(c.quotient) && state.hi == sx32(c.remainder) && state.hi1 == 123 && state.lo1 == 456,
              "DIV/DIVU quotient, remainder, overflow, or zero handling failed");
    }
    auto product = at(0x7e00); product.gpr[1] = {0xffffffffU,0xabcdef}; product.set(2,2);
    nt_generated_run(product,bus,1);
    check(product.lo == 0xfffffffffffffffeULL && product.hi == ~std::uint64_t(0) &&
          product.gpr[1].lo == product.lo && product.gpr[1].hi == 0xabcdef, "MULT alias/sign extension failed");
    product = at(0x7f00); product.set(1,0xffffffffU); product.set(2,0xffffffffU);
    nt_generated_run(product,bus,1);
    check(product.lo == 1 && product.hi == 0xfffffffffffffffeULL && product.gpr[1].lo == 1,
          "MULTU high-word sign extension failed");
}
void unaligned_doublewords(RamBus& bus) {
    constexpr std::uint64_t original = 0xaabbccddeeff1234ULL;
    constexpr std::uint64_t loaded = 0x0123456789abcdefULL;
    bus.store(0x9000,8,loaded);
    for (unsigned offset = 0; offset < 8; ++offset) {
        for (bool left : {false,true}) {
            auto state = at(left ? 0x7400 : 0x7500);
            state.set(1,0x9000 + offset); state.gpr[2] = {original,0x123456};
            auto expected = original;
            const auto count = left ? offset + 1 : 8 - offset;
            for (unsigned byte = 0; byte < count; ++byte) {
                const auto source = left ? byte : byte + offset;
                const auto destination = left ? byte + 7 - offset : byte;
                expected = (expected & ~(0xffULL << (destination * 8))) |
                           (((loaded >> (source * 8)) & 255) << (destination * 8));
            }
            nt_generated_run(state,bus,1);
            check(state.gpr[2].lo == expected && state.gpr[2].hi == 0x123456, "LDL/LDR merge or upper lane failed");
        }
    }
    bus.store(0x9008,8,0xfedcba9876543210ULL);
    auto pair = at(0x7600); pair.set(1,0x9003);
    nt_generated_run(pair,bus,2);
    check(pair.gpr[2].lo == 0x5432100123456789ULL, "Unaligned LDL/LDR pair failed");
    auto alias = at(0x7700); alias.set(2,0x9007); nt_generated_run(alias,bus,1);
    check(alias.gpr[2].lo == loaded, "LDL base/destination alias failed");
    auto fault = at(0x7400); fault.set(1,0x2000000); fault.set(2,original);
    const auto load_fault = nt_generated_run(fault,bus,1);
    check(load_fault.reason == Stop::memory_fault && !load_fault.detail.empty() && fault.gpr[2].lo == original,
          "Faulting LDL changed its destination");
    for (unsigned offset = 0; offset < 8; ++offset) {
        for (bool left : {false,true}) {
            bus.store(0x9000,8,loaded);
            auto state = at(left ? 0x7900 : 0x7a00);
            state.set(1,0x9000 + offset); state.set(2,original);
            auto expected = loaded;
            const auto count = left ? offset + 1 : 8 - offset;
            for (unsigned byte = 0; byte < count; ++byte) {
                const auto source = left ? byte + 7 - offset : byte;
                const auto destination = left ? byte : byte + offset;
                expected = (expected & ~(0xffULL << (destination * 8))) |
                           (((original >> (source * 8)) & 255) << (destination * 8));
            }
            nt_generated_run(state,bus,1);
            check(bus.load(0x9000,8) == expected && state.gpr[2].lo == original, "SDL/SDR byte preservation failed");
        }
    }
    bus.store(0x9000,8,0); bus.store(0x9008,8,0);
    pair = at(0x7b00); pair.set(1,0x9003); pair.set(2,original);
    nt_generated_run(pair,bus,2);
    for (unsigned byte = 0; byte < 8; ++byte)
        check(bus.load(0x9003 + byte,1) == ((original >> (byte * 8)) & 255), "SDL/SDR unaligned pair failed");
    check(bus.load(0x9002,1) == 0 && bus.load(0x900b,1) == 0, "SDL/SDR corrupted adjacent bytes");
    fault = at(0x7900); fault.set(1,0x2000000);
    check(nt_generated_run(fault,bus,1).reason == Stop::memory_fault, "Invalid SDL did not fault");
}
void leading_sign_bits(RamBus& bus) {
    const auto reference_count = [](std::uint32_t value) {
        unsigned count = 0;
        for (int bit = 30; bit >= 0 && ((value >> bit) & 1) == (value >> 31); --bit) ++count;
        return count;
    };
    for (unsigned bit = 0; bit < 32; ++bit) {
        for (const auto low : {0U,~0U,1U << bit,~(1U << bit)}) {
            const auto high = low ^ 0xa5a5a5a5U;
            auto state = at(0x7200);
            state.gpr[2] = {std::uint64_t(low) | (std::uint64_t(high) << 32),0x123456789abcdef0ULL};
            nt_generated_run(state,bus,1);
            check(state.gpr[2].lo == (reference_count(low) | (std::uint64_t(reference_count(high)) << 32)) &&
                  state.gpr[2].hi == 0x123456789abcdef0ULL, "PLZCW lane count, alias, or upper preservation failed");
        }
    }
    auto zero = at(0x7300); nt_generated_run(zero,bus,1);
    check(zero.gpr[0].lo == 0 && zero.gpr[0].hi == 0, "PLZCW wrote register zero");
}
void extended_operations(RamBus& bus) {
    auto cache = at(0x8400); cache.set(1,0x9000); cache.gpr[24] = {123,456};
    bus.store(0x9000,4,0x12345678);
    check(nt_generated_run(cache,bus,3).reason == Stop::budget && cache.pc == 0x840c &&
          cache.gpr[24].lo == 123 && cache.gpr[24].hi == 456 && bus.load(0x9000,4) == 0x12345678,
          "Coherent data cache operations changed visible state");
    check(nt_generated_run(cache,bus,1).reason == Stop::unsupported,"Unimplemented cache-tag operation accepted");
    auto s = at(0x6400); s.gpr[2] = {0x123456789abc4321,0x0123456789ab8765};
    nt_generated_run(s,bus,1);
    check(s.gpr[2].lo == 0x4321432143214321 && s.gpr[2].hi == 0x8765876587658765, "PCPYH replication/alias failed");
    s = at(0x6500); s.gpr[1] = {11,22}; s.gpr[2] = {33,44}; nt_generated_run(s,bus,1);
    check(s.gpr[1].lo == 33 && s.gpr[1].hi == 11, "PCPYLD source alias failed");
    s = at(0x6600); s.gpr[1] = {11,22}; s.gpr[2] = {33,44}; nt_generated_run(s,bus,1);
    check(s.gpr[2].lo == 22 && s.gpr[2].hi == 44, "PCPYUD source alias failed");
    s = at(0x6700); nt_generated_run(s,bus,1);
    check(s.cop0_status == 0x10000, "EI kernel permission failed");
    s.pc = 0x6800; nt_generated_run(s,bus,1); check(s.cop0_status == 0, "DI failed");
    s = at(0x6700); s.cop0_status = 0x10; nt_generated_run(s,bus,1);
    check(s.cop0_status == 0x10, "EI incorrectly allowed in user mode");
    s.pc = 0x6700; s.cop0_status |= 0x20000; nt_generated_run(s,bus,1);
    check(s.cop0_status == 0x30010, "EI EDI permission failed");
    s = at(0x6a00); s.gpr[2].lo = 0xffffffff; nt_generated_run(s,bus,1);
    s.pc = 0x6900; nt_generated_run(s,bus,1);
    check(s.gpr[2].lo == 0xfffffffff0c79c1fULL, "COP0 status transfer/mask failed");
    s = at(0x6b00); s.cop0_status = 2; s.cop0_epc = 0x1000;
    nt_generated_run(s,bus,1);
    check(s.pc == 0x1000 && s.cop0_status == 0 && !s.delay_slot,
          "ERET did not restore EPC and clear EXL without a delay slot");
    s = at(0x6b00); s.cop0_status = 6; s.cop0_epc = 0x1000; s.cop0_error_epc = 0x1100;
    nt_generated_run(s,bus,1);
    check(s.pc == 0x1100 && s.cop0_status == 2 && !s.delay_slot,
          "ERET did not prioritize ErrorEPC and preserve EXL");
}
}
int main() {
    try {
        RamBus bus;
        for (const auto& [pc, word] : fixture::words()) bus.store(pc,4,word);
        test(bus);
        handlers(bus);
        extended_operations(bus);
        thread_switches(bus);
        multiply_divide(bus);
        unaligned_doublewords(bus);
        leading_sign_bits(bus);
        dma_interrupt_mask();
        sif_interrupt_delivery();
        semaphore_switches(bus);
        std::cout << "Compiled native instruction, memory, control-flow and stop-boundary tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
