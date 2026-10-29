#pragma once
#include <nt/ee_interrupts.hpp>
#include <nt/ee_timers.hpp>
#include <nt/ee_completions.hpp>
#include <nt/ee_sif_link.hpp>
#include <nt/ee_sif_outbound.hpp>
namespace nt::ee {
class NativePlatform {
public:
    explicit NativePlatform(NativeTimers::Clock clock=nullptr) : timers(clock) { timers.connect_events(); }
    NativeInterrupts interrupts;
    NativeTimers timers;
    NativeCompletions completions;
    NativeSifReceive sif_receive;
    NativeSifLink sif_link{sif_receive};
    NativeSifOutbound sif_outbound;
    void attach(Memory& memory) { memory.interrupts=&interrupts; memory.completions=&completions; memory.sif_link=&sif_link; memory.sif_outbound=&sif_outbound; memory.io={this,read,write,poll}; }
private:
    static void poll(void* context) {
        auto& self=*static_cast<NativePlatform*>(context);
        auto events=self.timers.poll();
        while (events) {
            const auto line=static_cast<unsigned>(std::countr_zero(events));
            self.interrupts.raise(line); events&=events-1;
        }
    }
    static bool event_address(std::uint32_t address) { return address==0x1000f000 || address==0x1000f010; }
    static IoRead read(void* context,std::uint32_t address) {
        auto& self=*static_cast<NativePlatform*>(context);
        if (NativeSifReceive::maps(address)) return self.sif_receive.read(address);
        if (address==0x1000e010) return {Stop::budget,self.completions.status()};
        if (event_address(address)) return self.interrupts.read(address);
        const auto io=self.timers.interface(); return io.read32(io.context,address);
    }
    static Stop write(void* context,std::uint32_t address,std::uint32_t value) {
        auto& self=*static_cast<NativePlatform*>(context);
        if (NativeSifReceive::maps(address)) return self.sif_receive.write(address,value);
        if (address==0x1000e010) { self.completions.acknowledge(value); return Stop::budget; }
        if (event_address(address)) return self.interrupts.write(address,value);
        const auto io=self.timers.interface(); return io.write32(io.context,address,value);
    }
};
}
