#include "native_fixture.hpp"
#include <nt/runtime.hpp>
#include <nt/analysis.hpp>
#include <nt/kernel.hpp>
#include <nt/system_bus.hpp>
#include <iostream>
#include <limits>
#include <cmath>

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
void timer_interrupt_delivery() {
    nt::hardware::SystemBus bus;
    nt::kernel::Kernel kernel;
    for (const auto& [pc,word] : fixture::words()) bus.store(pc,4,word);
    State registration;
    registration.set(3,0x10); registration.set(4,9); registration.set(5,0x9000);
    registration.set(6,0); registration.set(7,0x100600); registration.set(28,0x12345678);
    check(kernel.service(registration,bus),"Timer handler registration failed");
    registration.set(3,0x74); registration.set(4,0x12c); registration.set(5,0x9300);
    check(kernel.service(registration,bus),"Timer-3 alias registration failed");
    // Two real timer overflows; timer zero stays masked while timer three is enabled.
    for (auto base : {0x10000000U,0x10001800U}) {
        bus.store(base,4,0xffff); bus.store(base+0x10,4,0x280);
    }
    bus.advance_cycles(2);
    auto state = at(0x9200); kernel.initialize_cpu(state); state.set(29,0x1fff000);
    state.cop0_status &= ~0x400U;
    check(!kernel.enter_interrupt(state,bus),"INTC ignored CPU mask");
    state.cop0_status |= 0x400U; state.delay_slot = true;
    check(!kernel.enter_interrupt(state,bus),"INTC interrupted a delay slot");
    state.delay_slot = false;
    auto result = nt::kernel::run(nt_generated_run,state,bus,kernel,100);
    check(result.stop.reason == Stop::unsupported && kernel.completed_interrupts() == 1 &&
          bus.load(0x800,4) == 12 && bus.load(0x804,4) == 0 && bus.load(0x808,4) == 0,
          "Timer-3 alias callback ABI or return failed");
    check(bus.load(0x1000f000,4) == (1U<<9) && bus.load(0x1000f010,4) == (1U<<12),
          "INTC acknowledgement erased a masked source or changed masks");
    bus.store(0x1000f010,4,1U<<9);
    result = nt::kernel::run(nt_generated_run,state,bus,kernel,2);
    check(result.stop.reason == Stop::budget && kernel.interrupt_active() && (state.cop0_status & 3) == 2,
          "INTC callback suspension or exception-level status failed");
    result = nt::kernel::run(nt_generated_run,state,bus,kernel,100);
    check(result.stop.reason == Stop::unsupported && state.pc == 0x9204 && kernel.completed_interrupts() == 2 &&
          bus.load(0x100600,4) == 9 && bus.load(0x100604,4) == 0x12345678 && bus.load(0x100608,4) == 99 &&
          state.gpr[29].lo == 0x1fff000 && state.fpr[3] == 0 && state.cop0_status == 0x10c01,
          "INTC chain execution or context restoration failed");
    registration.set(3,0x12); registration.set(4,5); registration.set(5,0x9000);
    registration.set(6,0); registration.set(7,0x100700);
    check(kernel.service(registration,bus),"Concurrent DMA registration failed");
    bus.store(0x1000e000,4,1); bus.store(0x1000e010,4,1U<<21);
    bus.store(0x1000c000,4,0x100); // Empty normal SIF0 transfer completes immediately.
    for (auto base : {0x10000000U,0x10001800U}) bus.store(base,4,0xffff);
    bus.advance_cycles(2);
    check(kernel.enter_interrupt(state,bus) && state.gpr[4].lo == 5 && bus.interrupt_lines() == 0x400,
          "DMAC did not take priority over pending INTC sources");
    result = nt::kernel::run(nt_generated_run,state,bus,kernel,100);
    check(result.stop.reason == Stop::unsupported && kernel.completed_interrupts() == 4 && !bus.interrupt_pending(),
          "Concurrent controller dispatch did not complete and resume");
    nt::kernel::Kernel unregistered;
    bus.store(0x10000000,4,0xffff); bus.advance_cycles(2);
    check(!unregistered.enter_interrupt(state,bus) && (bus.load(0x1000f000,4) & (1U<<9)),
          "Unknown INTC handler silently consumed an interrupt");
}
void scanout_interrupt_delivery(unsigned line) {
    nt::hardware::SystemBus bus;
    nt::kernel::Kernel kernel;
    for (const auto& [pc,word] : fixture::words()) bus.store(pc,4,word);
    State registration;
    registration.set(3,0x10); registration.set(4,line); registration.set(5,0x9000);
    registration.set(6,0); registration.set(7,0x100600); registration.set(28,0x12345678);
    check(kernel.service(registration,bus),"VBlank handler registration failed");
    registration.set(3,2); registration.set(4,1); registration.set(5,3); registration.set(6,0);
    check(kernel.service(registration,bus),"PAL CRT setup failed");
    bus.store(0x1000f010,4,1U << line);
    if (line == 0) bus.store(0x12001010,8,0x7700);
    bus.advance_cycles(line == 0 ? 5492442 : line == 2 ? 5435818 : 5898240);
    auto state = at(0x9200); kernel.initialize_cpu(state); state.set(29,0x1fff000);
    const auto result = nt::kernel::run(nt_generated_run,state,bus,kernel,100);
    check(result.stop.reason == Stop::unsupported && kernel.completed_interrupts() == 1 &&
          bus.load(0x100600,4) == line && bus.load(0x100604,4) == 0x12345678 &&
          state.gpr[29].lo == 0x1fff000 && !bus.interrupt_pending(),
          "Scanout interrupt did not execute generated guest handler and restore context");
}
void vu_initialization() {
    nt::hardware::SystemBus bus;
    for (const auto& [pc,word] : fixture::words()) bus.store(pc,4,word);
    for (unsigned reg=0;reg<16;++reg) {
        auto state=at(0xab00+reg*16); state.set(1,0xffff1234U+reg); state.gpr[2].hi=0x5678;
        check(nt_generated_run(state,bus,2).reason == Stop::budget &&
              state.gpr[2].lo == (reg ? 0x1234U+reg : 0) && state.gpr[2].hi == 0x5678,
              "VU integer control transfer width/constant register differs");
    }
    for (const auto value : {0xc0cU,0xc0eU,0xe0cU,0xe0eU}) {
        auto state = at(0x9b00); state.set(1,value); state.gpr[2].hi = 0x12345678;
        auto expected = value & 0xc0c;
        if (value & 2) expected &= ~0xffU;
        if (value & 0x200) expected &= ~0xff00U;
        check(nt_generated_run(state,bus,2).reason == Stop::budget && state.gpr[2].lo == expected &&
              state.gpr[2].hi == 0x12345678,"Compiled VU reset/control roundtrip differs");
    }
    auto state = at(0x9b10);
    check(nt_generated_run(state,bus,1).reason == Stop::budget && state.gpr[3].lo == 0,"Idle VPU status differs");
    state = at(0x9b20); state.set(1,0x100);
    check(nt_generated_run(state,bus,1).reason == Stop::memory_fault && state.pc == 0x9b20 && state.executed == 0,
          "VU microprogram execution was silently accepted");
}
void vu_subtraction() {
    nt::hardware::SystemBus bus;
    for (const auto& [pc,word] : fixture::words()) bus.store(pc,4,word);
    const auto pack = [](std::array<std::uint32_t,4> v) {
        return Register{v[0] | (std::uint64_t(v[1])<<32),v[2] | (std::uint64_t(v[3])<<32)};
    };
    // One shared arithmetic milestone: vector/broadcast forms, each mask, and
    // an aliased destination containing four distinct broadcast components.
    const std::array<float,4> left{1,-2,4,-8}, right{2,4,-8,16};
    for (unsigned operation=0;operation<2;++operation)
        for (unsigned broadcast=0;broadcast<5;++broadcast)
            for (unsigned mask=0;mask<16;++mask) {
                std::array<std::uint32_t,4> av{},bv{},expected{};
                for (unsigned i=0;i<4;++i) {
                    av[i]=std::bit_cast<std::uint32_t>(left[i]);
                    bv[i]=std::bit_cast<std::uint32_t>(right[i]);
                    const float operand=right[broadcast<4 ? broadcast : i];
                    expected[i]=(mask&(8U>>i)) ? std::bit_cast<std::uint32_t>(left[i]+(operation ? -operand : operand)) : bv[i];
                }
                bus.write_cop2_vector(1,pack(av)); bus.write_cop2_vector(2,pack(bv));
                auto state=at(0xa000+((operation*5+broadcast)*16+mask)*16);
                check(nt_generated_run(state,bus,1).reason == Stop::budget &&
                      bus.read_cop2_vector(2).lo == pack(expected).lo && bus.read_cop2_vector(2).hi == pack(expected).hi,
                      "VU vector/broadcast add-subtract alias/mask differs");
            }
    const std::array<std::uint32_t,4> a{0x3f800000,0x40000000,0x00800001,0x80000000};
    const std::array<std::uint32_t,4> b{0x40000000,0x3f800000,0x00800000,0};
    const std::array<std::uint32_t,4> difference{0xbf800000,0x3f800000,0,0x80000000};
    for (unsigned mask=0;mask<16;++mask) {
        auto state=at(0x9d00+mask*32); state.gpr[1]=pack(a); state.gpr[2]=pack(b);
        auto expected=b;
        for (unsigned lane=0;lane<4;++lane) if (mask&(8U>>lane)) expected[lane]=difference[lane];
        check(nt_generated_run(state,bus,4).reason == Stop::budget &&
              state.gpr[3].lo == pack(expected).lo && state.gpr[3].hi == pack(expected).hi,
              "Compiled masked VSUB/aliased vector transfer differs");
        const auto flags = ((mask&8)?0x80U:0U) | ((mask&2)?0x202U:0U) | ((mask&1)?0x11U:0U);
        check(bus.read_cop2_control(17) == flags,"VSUB MAC lane flags differ");
    }
    check(bus.read_cop2_control(16) == 0x1c7,"VU current/sticky STATUS aggregation differs");
    {
        auto state=at(0x9f00); state.gpr[1]={~0ULL,~0ULL};
        check(nt_generated_run(state,bus,3).reason == Stop::budget && state.gpr[3].lo == 0 &&
              state.gpr[3].hi == 0x3f80000000000000ULL && bus.read_cop2_control(17) == 15,
              "VF0 was modified or destination-zero flags were skipped");
        check(bus.read_cop2_control(16) == 0x1c1,"VU sticky flags were lost");
    }
    // Independent exact-double oracle: exponent separation <= 20 keeps every
    // subtraction exact in double. Adjust a rounded float toward zero if needed.
    std::uint32_t seed=0x12345678;
    for (unsigned sample=0;sample<512;++sample) {
        const auto next = [&] { seed=seed*1664525U+1013904223U; return seed; };
        const auto left=(next()&0x807fffffU)|((100U+sample%21)<<23);
        const auto right=(next()&0x807fffffU)|((100U+(sample*7)%21)<<23);
        const double exact=double(std::bit_cast<float>(left))-double(std::bit_cast<float>(right));
        auto expected=std::bit_cast<std::uint32_t>(static_cast<float>(exact));
        if (std::abs(double(std::bit_cast<float>(expected))) > std::abs(exact)) --expected;
        auto state=at(0x9e00); state.gpr[1]=pack({left,0,0,0}); state.gpr[2]=pack({right,0,0,0});
        check(nt_generated_run(state,bus,4).reason == Stop::budget &&
              static_cast<std::uint32_t>(state.gpr[3].lo) == expected,"Finite VSUB disagrees with exact oracle");
    }
    for (const auto pair : {std::array<std::uint32_t,3>{0x3f800000,0x30800000,0x3f7fffff},
                            std::array<std::uint32_t,3>{0x3f800000,0xb0800000,0x3f800000},
                            std::array<std::uint32_t,3>{0x3f800000,0x00800000,0x3f7fffff},
                            std::array<std::uint32_t,3>{0,0x80000001,0}}) {
        auto state=at(0x9e00); // x lane only; disabled lanes contain unsupported exponents.
        state.gpr[1]=pack({pair[0],0x7fc00000,0x7fc00000,0x7fc00000});
        state.gpr[2]=pack({pair[1],0x7fc00000,0x7fc00000,0x7fc00000});
        check(nt_generated_run(state,bus,4).reason == Stop::budget &&
              static_cast<std::uint32_t>(state.gpr[3].lo) == pair[2],"VU rounding/denormal handling differs");
    }
    for (const auto pair : {std::array<std::uint32_t,2>{0x7fc00000,0},
                            std::array<std::uint32_t,2>{0x7f7fffff,0xff7fffff}}) {
        auto state=at(0x9ee0);
        state.gpr[1]=pack({0x3f800000,pair[0],0,0}); state.gpr[2]=pack({0,pair[1],0,0});
        const auto flags=bus.read_cop2_control(17), status=bus.read_cop2_control(16);
        check(nt_generated_run(state,bus,4).reason == Stop::memory_fault && state.pc == 0x9ee8 && state.executed == 2 &&
              bus.read_cop2_vector(2).lo == state.gpr[2].lo && bus.read_cop2_control(17) == flags &&
              bus.read_cop2_control(16) == status,"Unsupported VSUB partially committed lanes/flags");
    }
    auto state=at(0x9f20); state.set(1,0x180000); state.set(2,0x180020);
    bus.store_quad(0x180000,{0x1122334455667788ULL,0x8877665544332211ULL});
    check(nt_generated_run(state,bus,2).reason == Stop::budget &&
          bus.load_quad(0x180020).lo == 0x1122334455667788ULL &&
          bus.load_quad(0x180020).hi == 0x8877665544332211ULL,"LQC2/SQC2 lost vector bits");
    const auto flags = bus.read_cop2_control(17);
    bus.write_cop2_vector(5,{~0ULL,~0ULL}); state=at(0x9f30);
    check(nt_generated_run(state,bus,1).reason == Stop::budget &&
          bus.read_cop2_vector(5).lo == 0xffffffff55667788ULL &&
          bus.read_cop2_vector(5).hi == 0xffffffff44332211ULL && bus.read_cop2_control(17) == flags,
          "VMOVE changed disabled lanes or flags");
    check(nt_generated_run(state,bus,1).reason == Stop::budget &&
          bus.read_cop2_vector(5).lo == 0x44332211ffffffffULL &&
          bus.read_cop2_vector(5).hi == 0x55667788ffffffffULL && bus.read_cop2_control(17) == flags,
          "Aliased VMR32 did not rotate all lanes or preserved flags incorrectly");
}
void test(RamBus& bus) {
    for (unsigned upper=0;upper<2;++upper) {
        for (unsigned kind=0;kind<3;++kind) {
            auto state=at(0xb600+upper*64+kind*16);
            state.gpr[1]={0x1716151413121110ULL,0x2726252423222120ULL};
            state.gpr[2]={0x0706050403020100ULL,0x3736353433323130ULL};
            const auto a=state.gpr[1],b=state.gpr[2];
            std::array<std::uint8_t,16> expected{};
            const unsigned bytes=1U<<kind;
            for (unsigned index=0;index<16;++index) {
                const bool left=((index/bytes)&1)!=0;
                const auto source=left ? a : b;
                const auto offset=(index/(bytes*2))*bytes+index%bytes;
                expected[index]=static_cast<std::uint8_t>((upper ? source.hi : source.lo)>>(offset*8));
            }
            check(nt_generated_run(state,bus,1).reason==Stop::budget,"Packed interleave unsupported");
            for (unsigned index=0;index<16;++index)
                check(static_cast<std::uint8_t>((index<8 ? state.gpr[2].lo : state.gpr[2].hi)>>((index%8)*8))==expected[index],
                      "Packed interleave lane order or aliased destination differs");
        }
    }
    for (const auto pair : {std::array<std::uint32_t,2>{0,0x80000000}, {0x3f800000,0xbf800000},
                            {0xbf800000,0xc0000000}, {1,0}, {0x80000000,0x80000001},
                            {0x7fffffff,0x7f800000}, {0xff800000,0xffffffff}}) {
        for (unsigned reverse=0;reverse<2;++reverse) {
            auto state=at(0xb580); state.fpr[1]=pair[reverse]; state.fpr[2]=pair[1-reverse]; state.fcr31=0xffffffff;
            check(nt_generated_run(state,bus,1).reason==Stop::budget && state.fpr[1]==pair[0] &&
                  state.fcr31==0xffff3fff,"MAX.S raw ordering, alias or flags differ");
            state.pc=0xb584; state.fpr[1]=pair[reverse]; state.fpr[2]=pair[1-reverse];
            check(nt_generated_run(state,bus,1).reason==Stop::budget && state.fpr[2]==pair[1],"MIN.S raw ordering or alias differs");
        }
    }
    for (const auto pair : {std::array<std::uint32_t,2>{0x3ff33333,1}, {0xbff33333,0xffffffff},
                            {0x80000001,0}, {0x3f000000,0}, {0x4effffff,0x7fffff80},
                            {0x4f000000,0x7fffffff}, {0xcf000000,0x80000000},
                            {0x7fc12345,0x7fffffff}, {0xffc12345,0x80000000}}) {
        auto state=at(0xb560); state.fpr[1]=pair[0]; state.fcr31=0xffffffff;
        check(nt_generated_run(state,bus,1).reason==Stop::budget && state.fpr[1]==pair[1] && state.fcr31==0xffffffff,
              "CVT.W.S truncation, signed saturation, alias or flags differ");
    }
    for (unsigned subtract=0;subtract<2;++subtract) {
        for (const auto values : {std::array<std::uint32_t,4>{0x3f800000,0x3f000000,0x3fc00000,0},
                                  {0xbf800000,0x3f800000,0,0}, {0x80000000,0x80000000,0x80000000,0},
                                  {0x00800001,0x80800000,0,0x4008}, {0x3f800000,0x80000001,0x3f800000,0}}) {
            auto state=at(0xb540+subtract*16); state.fpr[1]=values[0];
            state.fpr[2]=values[1]^(subtract ? 0x80000000U : 0); state.fcr31=0x83c070;
            check(nt_generated_run(state,bus,1).reason==Stop::budget && state.fpr[2]==values[2] &&
                  state.fcr31==(0x830070U|values[3]),"Scalar add/sub alias, result or flags differ");
        }
        auto state=at(0xb540+subtract*16); state.fpr[1]=0x7f7fffff;
        state.fpr[2]=0x7f7fffff^(subtract ? 0x80000000U : 0); state.fcr31=0x12345678;
        const auto original=state.fpr;
        check(nt_generated_run(state,bus,1).reason==Stop::memory_fault && state.fpr==original &&
              state.fcr31==0x12345678 && state.executed==0,"Scalar add/sub overflow partially committed");
    }
    std::uint32_t division_seed=0x9137a5;
    for (unsigned sample=0;sample<512;++sample) {
        const auto operand=[&]() {
            division_seed=division_seed*1664525U+1013904223U;
            return (division_seed&0x807fffffU) | ((110U+(division_seed>>24)%31U)<<23);
        };
        auto state=at(0xb520); state.fpr[1]=operand(); state.fpr[2]=operand();
        const auto expected=static_cast<float>(static_cast<double>(std::bit_cast<float>(state.fpr[1])) /
                                               static_cast<double>(std::bit_cast<float>(state.fpr[2])));
        check(nt_generated_run(state,bus,1).reason==Stop::budget && state.fpr[2]==std::bit_cast<std::uint32_t>(expected),
              "Finite DIV.S corpus differs from nearest rounding");
    }
    for (const auto values : {std::array<std::uint32_t,4>{0x3f800000,0x40400000,0x3eaaaaab,0},
                              {0xbf800000,0x40000000,0xbf000000,0}, {0,0,0x7f7fffff,0x20040},
                              {0xbf800000,0,0xff7fffff,0x10020}, {0x80000001,0x3f800000,0x80000000,0},
                              {0x00800000,0x40000000,0,0}}) {
        auto state=at(0xb520); state.fpr[1]=values[0]; state.fpr[2]=values[1]; state.fcr31=0x83c018;
        check(nt_generated_run(state,bus,1).reason==Stop::budget && state.fpr[2]==values[2] &&
              state.fcr31==(0x80c018U|values[3]),"DIV.S result, alias or exception flags differ");
    }
    for (const auto pair : {std::array<std::uint32_t,2>{0x7f800000,0x3f800000}, {0x7f7fffff,0x00800000}}) {
        auto state=at(0xb520); state.fpr[1]=pair[0]; state.fpr[2]=pair[1]; state.fcr31=0xffffffff;
        check(nt_generated_run(state,bus,1).reason==Stop::memory_fault && state.executed==0 &&
              state.fpr[2]==pair[1] && state.fcr31==0xffffffff,"DIV.S failure partially committed state");
    }
    for (const auto pair : {std::array<std::uint32_t,2>{0,0}, {1,0x3f800000}, {0xffffffff,0xbf800000},
                            {0x7fffffff,0x4effffff}, {0x80000000,0xcf000000},
                            {0x01000003,0x4b800001}, {0xfefffffd,0xcb800001}}) {
        auto state=at(0xb500); state.fpr[1]=pair[0]; state.fcr31=0xffffffff;
        check(nt_generated_run(state,bus,2).reason==Stop::budget && state.pc==0xb50c &&
              !state.delay_slot && state.fpr[1]==pair[1] && state.fcr31==0xffffffff,
              "CVT.S.W chop rounding, alias, flags or delay continuation differs");
    }
    bus.write_performance(0,0x12345678);
    for (unsigned selector=0;selector<64;++selector) {
        auto state=at(0xb000+selector*16); state.set(1,selector&1 ? 0xfedcba98 : selector ? 0xffffffff : 0x12345678);
        state.gpr[2].hi=0xaabbccdd;
        check(nt_generated_run(state,bus,2).reason==Stop::budget &&
              state.gpr[2].lo==sx32(selector&1 ? 0xfedcba98 : 0x12345678) && state.gpr[2].hi==0xaabbccdd,
              "Performance register selector/sign extension differs");
    }
    auto performance=at(0xb000); performance.set(1,0x80000040);
    check(nt_generated_run(performance,bus,1).reason==Stop::memory_fault && performance.executed==0 &&
          bus.read_performance(0)==0x12345678,"Unsupported performance counting changed control state");
    bus.write_performance(1,0); bus.write_performance(3,0);
    bus.write_performance(0,0x80031024); // Kernel cycles and coherent instruction-cache misses.
    auto counted=at(0xb014); counted.cop0_status=0;
    check(nt_generated_run(counted,bus,1).reason==Stop::budget && counted.gpr[2].lo==0 &&
          bus.read_performance(1)==1 && bus.read_performance(3)==0,"Cycle counter/read ordering differs");
    bus.advance_performance(4,9); bus.advance_performance(0x10,9);
    check(bus.read_performance(1)==1,"Counter ignored ERL or privilege gate");
    bus.write_performance(1,0x7ffffffe); bus.advance_performance(0,1);
    bool overflow=false;
    try { bus.advance_performance(0,1); } catch (const MemoryFault&) { overflow=true; }
    check(overflow && bus.read_performance(1)==0x7fffffff,"Counter overflow silently wrapped");
    bus.write_performance(0,0);
    std::uint32_t multiply_seed=0x51480327;
    for (unsigned sample=0;sample<512;++sample) {
        const auto operand=[&]() {
            multiply_seed=multiply_seed*1664525U+1013904223U;
            return (multiply_seed & 0x807fffffU) | ((100U+(multiply_seed>>24)%51U)<<23);
        };
        auto state=at(0xae00); state.fpr[1]=operand(); state.fpr[2]=operand();
        const double exact=static_cast<double>(std::bit_cast<float>(state.fpr[1])) *
                           static_cast<double>(std::bit_cast<float>(state.fpr[2]));
        float expected=static_cast<float>(exact);
        if (std::abs(static_cast<double>(expected)) > std::abs(exact))
            expected=std::nextafter(expected,0.0F);
        check(nt_generated_run(state,bus,2).reason == Stop::budget &&
              state.fpr[1] == std::bit_cast<std::uint32_t>(expected),"Finite MUL.S corpus differs from exact-product chop rounding");
    }
    for (const auto values : {
        std::array<std::uint32_t,4>{0x3fc00000,0xc0200000,0xc0700000,0},
        std::array<std::uint32_t,4>{0x3f800001,0x3f800001,0x3f800002,0},
        std::array<std::uint32_t,4>{0x80000001,0x3f800000,0x80000000,0},
        std::array<std::uint32_t,4>{0x00800000,0x3f000000,0,0x4008},
        std::array<std::uint32_t,4>{0x80800000,0x00800000,0x80000000,0x4008}}) {
        auto state=at(0xae00); state.fpr[1]=values[0]; state.fpr[2]=values[1]; state.fcr31=0x80c070;
        check(nt_generated_run(state,bus,2).reason == Stop::budget && state.pc == 0xae0c &&
              !state.delay_slot && state.fpr[1] == values[2] && state.fpr[2] == values[1] &&
              state.fcr31 == (0x800070U | values[3]),"MUL.S alias, delay slot, result or flags differ");
    }
    for (const auto pair : {std::array<std::uint32_t,2>{0x7f800000,0},
                            std::array<std::uint32_t,2>{0x7f7fffff,0x40000000}}) {
        auto state=at(0xae00); state.fpr[1]=pair[0]; state.fpr[2]=pair[1]; state.fcr31=0xffffffff;
        check(nt_generated_run(state,bus,2).reason == Stop::memory_fault && state.pc == 0xae04 &&
              state.delay_slot && state.branch_pc == 0xae00 && state.executed == 1 &&
              state.fpr[1] == pair[0] && state.fcr31 == 0xffffffff,"MUL.S failure partially committed state");
    }
    for (unsigned kind=0;kind<4;++kind) {
        for (const auto pair : {std::array<std::uint32_t,2>{0xbf800000,0x3f800000},
                                std::array<std::uint32_t,2>{0x80000001,0},
                                std::array<std::uint32_t,2>{0x7fffffff,0x7f7fffff}}) {
            auto state=at(0xad00+kind*32); state.fpr[1]=pair[0]; state.fpr[2]=pair[1]; state.fcr31=0x1234;
            const bool equal=pair[0]!=0xbf800000, less=!equal;
            const bool condition=kind==1 ? equal : kind==2 ? less : kind==3;
            check(nt_generated_run(state,bus,1).reason == Stop::budget &&
                  state.fcr31 == (0x1234U | (condition ? 0x800000U : 0)),"Scalar compare condition/flags differ");
            const bool taken=(kind&1) ? condition : !condition;
            const bool annul=kind>=2 && !taken;
            check(nt_generated_run(state,bus,annul ? 1 : 2).reason == Stop::budget &&
                  state.gpr[4].lo == (annul ? 0 : 1) && state.pc == 0xad00+kind*32+(taken ? 16 : 12),
                  "BC1 branch target/delay-slot annul differs");
        }
    }
    for (const auto bits : {0U,0x80000000U,0xffc12345U,0x3f800000U}) {
        auto state=at(0xac00); state.fpr[3]=bits; state.fcr31=0xffffffff;
        check(nt_generated_run(state,bus,1).reason == Stop::budget && state.fpr[4] == bits && state.fcr31 == 0xffffffff,"MOV.S altered bits/flags");
        check(nt_generated_run(state,bus,2).reason == Stop::budget && state.fpr[4] == (bits&0x7fffffffU) &&
              state.fpr[5] == (bits|0x80000000U) && state.fcr31 == 0xffff3fff,"Scalar sign operations altered payload/flags");
    }
    for (const auto bits : {0U,0x80000000U,0x7fc12345U,0xffffffffU}) {
        bus.store(0x180000,4,bits); bus.store(0x180008,8,0xaabbccdd00000000ULL);
        auto state = at(0x9c00); state.set(1,0xa0180004); state.set(2,123);
        check(nt_generated_run(state,bus,2).reason == Stop::budget && state.fpr[0] == bits &&
              bus.load(0x180008,8) == (0xaabbccdd00000000ULL | bits) && state.gpr[2].lo == 123,
              "Raw FPR memory roundtrip changed bits, adjacent bytes or GPRs");
    }
    {
        auto state = at(0x9c00); state.set(1,0x180005); state.fpr[0] = 123;
        check(nt_generated_run(state,bus,1).reason == Stop::memory_fault && state.fpr[0] == 123 && state.executed == 0,
              "Misaligned LWC1 changed state");
        state = at(0x9c10); state.set(1,0x180001); state.fpr[31] = 42;
        check(nt_generated_run(state,bus,2).reason == Stop::memory_fault && state.delay_slot && state.pc == 0x9c14 && state.executed == 1,
              "SWC1 delay-slot fault lost branch state");
    }
    for (auto pc : {0x9a00U,0x9a10U,0x9a20U}) {
        auto state=at(pc); state.set(1,pc==0x9a10 ? 0x8000000000000000ULL : 0x7fffffffffffffffULL);
        state.set(2,1); state.gpr[3]={17,23};
        check(nt_generated_run(state,bus,1).reason==Stop::unsupported && state.pc==pc && state.executed==0 && state.gpr[3].lo==17,
              "Signed doubleword overflow changed guest state");
        state.set(1,pc==0x9a10 ? 0 : 0x8000000000000000ULL);
        check(nt_generated_run(state,bus,1).reason==Stop::budget && state.gpr[3].hi==23 &&
              state.gpr[3].lo==(pc==0x9a10 ? ~0ULL : 0x8000000000000001ULL),"Signed doubleword result failed");
    }
    for (auto pc : {0x9900U,0x9910U,0x9920U}) {
        auto state=at(pc); state.gpr[1]={0x0706050403020100ULL,0x0f0e0d0c0b0a0908ULL};
        state.gpr[2]={~state.gpr[1].lo,~state.gpr[1].hi};
        nt_generated_run(state,bus,1);
        const auto expected=pc==0x9900 ? 0x0e0c0a0806040200ULL : pc==0x9910 ? 0x0d0c090805040100ULL : 0x0b0a090803020100ULL;
        check(state.gpr[2].lo==~expected && state.gpr[2].hi==expected,"Packed even-lane selection or aliased operands failed");
    }
    auto packed = at(0x9800); packed.gpr[1]={0x00000000ffffffffULL,0x80000000000000f8ULL};
    nt_generated_run(packed,bus,2);
    check(packed.gpr[1].lo==0xffff && packed.gpr[1].hi==0x000080000000001fULL &&
          packed.gpr[2].lo==0x80f8f8f8 && packed.gpr[2].hi==0x80000000000000f8ULL,
          "Packed five-bit conversion lost lane placement or alpha");
    for (auto pc : {0x9700U,0x9710U,0x9720U}) {
        auto state=at(pc); state.gpr[1]={0,~0ULL}; state.gpr[2]={0x100,0xfffffffffffffffeULL};
        nt_generated_run(state,bus,1);
        check(state.gpr[1].lo==(pc==0x9700 ? 0xffffffffffff00ffULL : pc==0x9710 ? 0xffffffffffff0000ULL : 0xffffffff00000000ULL) &&
              state.gpr[1].hi==(pc==0x9700 ? 0xffffffffffffff00ULL : pc==0x9710 ? 0xffffffffffff0000ULL : 0xffffffff00000000ULL),
              "Packed equality lane width, high half or operand aliasing failed");
    }
    for (auto pc : {0x9600U,0x9610U,0x9620U}) {
        auto state=at(pc); state.set(1,0x7fffffff); state.set(2,pc==0x9610 ? ~0ULL : 1);
        state.gpr[3]={17,23};
        const auto stop=nt_generated_run(state,bus,1);
        check(stop.reason==Stop::unsupported && !stop.detail.empty() && state.pc==pc && state.executed==0 && state.gpr[3].lo==17,
              "Signed arithmetic overflow did not preserve the fault boundary");
        state.set(1,pc==0x9610 ? 0x80000001 : 0x80000000); state.set(2,1);
        check(nt_generated_run(state,bus,1).reason==Stop::budget && state.gpr[3].hi==23 &&
              state.gpr[3].lo==sx32(pc==0x9610 ? 0x80000000 : 0x80000001),"Signed arithmetic result failed");
    }
    for (unsigned offset=0;offset<4;++offset) {
        constexpr std::uint64_t original=0x12345678aabbccddULL;
        for (bool left : {false,true}) {
            auto state=at(left ? 0x9500 : 0x9510); state.set(1,0xa000+offset); state.gpr[2]={original,0xfeed};
            bus.store(0xa000,4,0x80706050);
            std::uint32_t expected=static_cast<std::uint32_t>(original);
            for(unsigned i=0;i<4;++i) {
                if (left && i<=offset) {
                    const auto shift=(3-offset+i)*8;
                    expected=(expected & ~(255U<<shift)) | (static_cast<std::uint32_t>(bus.load(0xa000+i,1))<<shift);
                } else if (!left && i>=offset) {
                    const auto shift=(i-offset)*8;
                    expected=(expected & ~(255U<<shift)) | (static_cast<std::uint32_t>(bus.load(0xa000+i,1))<<shift);
                }
            }
            nt_generated_run(state,bus,1);
            check(state.gpr[2].lo==(left || !offset ? sx32(expected) : (original&0xffffffff00000000ULL)|expected) && state.gpr[2].hi==0xfeed,
                  "Unaligned word load byte selection or upper bits failed");
            state=at(left ? 0x9520 : 0x9530); state.set(1,0xa000+offset); state.set(2,original);
            nt_generated_run(state,bus,1);
            for(unsigned i=0;i<4;++i) {
                const auto value=left && i<=offset ? (original>>((3-offset+i)*8))&255 :
                    !left && i>=offset ? (original>>((i-offset)*8))&255 : (0x80706050U>>(i*8))&255;
                check(bus.load(0xa000+i,1)==value,"Unaligned word store changed the wrong byte");
            }
        }
        for(unsigned i=0;i<8;++i) bus.store(0xa000+i,1,0x80+i);
        auto state=at(0x9540); state.set(1,0xa000+offset);
        nt_generated_run(state,bus,2);
        std::uint32_t expected=0;
        for(unsigned i=0;i<4;++i) expected|=(0x80+offset+i)<<(8*i);
        check(state.gpr[2].lo==sx32(expected),"Unaligned word pair across a boundary failed");
    }
    // Variable-width timer arithmetic: masked counts, aliased operands and upper lanes.
    for (const auto shift : {0U,31U,32U,63U,64U,127U}) {
        auto state = at(0x9400); state.set(1,shift); state.gpr[2] = {0x8000000000000001ULL,0x1234};
        check(nt_generated_run(state,bus,3).reason == Stop::budget,"Variable doubleword shifts failed");
        const auto count = shift & 63;
        const auto left = 0x8000000000000001ULL << count;
        const auto arithmetic = (left >> count) | ((left >> 63) && count ? (~0ULL << (64-count)) : 0);
        check(state.gpr[2].lo == left && state.gpr[2].hi == 0x1234 &&
              state.gpr[3].lo == (left >> count) && state.gpr[4].lo == arithmetic,"Doubleword shift width or signed result failed");
    }
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
void sleep_interrupt_wakeup() {
    nt::hardware::SystemBus bus;
    nt::kernel::Kernel kernel;
    for (const auto& [pc,word] : fixture::words()) bus.store(pc,4,word);
    State s;
    const auto service=[&](unsigned call,unsigned a0,unsigned a1) {
        s.set(3,call); s.set(4,a0); s.set(5,a1);
        check(kernel.service(s,bus),"Sleep fixture service unsupported");
    };
    s.set(6,0x1000); s.set(7,0x3000); s.set(8,0x2000);
    service(0x3c,0,0x1fff000);
    s.set(6,0xffffffff); s.set(7,0);
    service(0x10,9,0xaf40); service(0x14,9,0);
    kernel.initialize_cpu(s); s.pc=0xaf00; s.set(3,0x32); s.set(29,0x1fffd60);
    s.gpr[16]={17,99}; s.fpr[4]=0x12345678;
    bus.write_performance(0,0x8000003e);
    auto result=nt::kernel::run(nt_generated_run,s,bus,kernel,20);
    check(result.stop.reason==Stop::budget && kernel.waiting() && s.executed==2 &&
          s.pc==0xaf10 && !s.delay_slot && s.gpr[16].lo==17 && bus.read_performance(1)==20,
          "Sleep executed mainline, lost delay continuation or skipped idle cycles");
    service(0x30,1,0x5000);
    check(bus.load(0x5000,4)==4 && bus.load(0x5024,4)==1,"Sleep wait status differs");
    bus.store(0x10000020,4,10); bus.store(0x10000010,4,0x180);
    result=nt::kernel::run(nt_generated_run,s,bus,kernel,100);
    check(result.stop.reason==Stop::unsupported && s.pc==0xaf14 && !kernel.waiting() &&
          kernel.completed_interrupts()==1 && s.executed==8 && s.gpr[16].lo==18 &&
          s.gpr[16].hi==99 && s.fpr[4]==0x12345678 && bus.read_performance(1)>20,
          "Timer wakeup lost context/counter progress or executed idle instructions");
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
        sleep_interrupt_wakeup();
        multiply_divide(bus);
        unaligned_doublewords(bus);
        leading_sign_bits(bus);
        dma_interrupt_mask();
        sif_interrupt_delivery();
        timer_interrupt_delivery();
        vu_initialization();
        vu_subtraction();
        for (const auto line : {0U,2U,3U}) scanout_interrupt_delivery(line);
        semaphore_switches(bus);
        std::cout << "Compiled native instruction, memory, control-flow and stop-boundary tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
