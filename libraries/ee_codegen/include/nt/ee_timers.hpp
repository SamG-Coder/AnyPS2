#pragma once
#include <nt/ee_native.hpp>
#include <array>
#include <chrono>

namespace nt::ee {
// Native elapsed-time timers. Event production is arithmetic over monotonic
// time; no per-tick loop or instruction-count clock is involved.
class NativeTimers {
public:
    using Clock=std::uint64_t (*)();
    explicit NativeTimers(Clock clock=nullptr) : clock_(clock ? clock : now) {}
    void connect_events(bool connected=true) { events_connected_=connected; }
    std::uint32_t poll() {
        const auto time=clock_();
        for (unsigned i=0;i<timers_.size();++i) advance(i,time);
        const auto events=events_; events_=0; return events;
    }
    NativeIo interface() { return {this,read,write}; }
private:
    struct Timer { std::uint32_t count=0,mode=0,target=0xffff,hold=0; std::uint64_t epoch=0,fraction=0; };
    static std::uint64_t now() {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    static bool decode(std::uint32_t address,unsigned& index,unsigned& field) {
        if (address<0x10000000U || address>=0x10002000U || address%16) return false;
        index=(address-0x10000000U)/0x800; field=address&0x7ff;
        return field==0 || field==0x10 || field==0x20 || (field==0x30 && index<2);
    }
    void advance(unsigned index,std::uint64_t time) {
        auto& timer=timers_[index];
        if (time<timer.epoch) return;
        const auto elapsed=time-timer.epoch; timer.epoch=time;
        if (!(timer.mode&0x80)) return;
        const auto frequency=147456000ULL/((timer.mode&3)==0 ? 1 : (timer.mode&3)==1 ? 16 : 256);
        const auto fractional=(elapsed%1000000000)*frequency+timer.fraction;
        const auto ticks=(elapsed/1000000000)*frequency+fractional/1000000000;
        timer.fraction=fractional%1000000000;
        if (!ticks) return;
        const auto compare_distance=timer.target>timer.count ? timer.target-timer.count :
            65536ULL-timer.count+timer.target;
        const bool compare=ticks>=compare_distance;
        const bool overflow=ticks>=65536ULL-timer.count;
        if (compare && (timer.mode&0x100) && !(timer.mode&0x400)) {
            timer.mode|=0x400; events_|=1U<<(9+index);
        }
        if (overflow && (timer.mode&0x200) && !(timer.mode&0x800)) {
            timer.mode|=0x800; events_|=1U<<(9+index);
        }
        timer.count=static_cast<std::uint32_t>((timer.count+ticks)&65535);
    }
    static IoRead read(void* context,std::uint32_t address) {
        auto& self=*static_cast<NativeTimers*>(context); unsigned index,field;
        if (!decode(address,index,field)) return {Stop::memory_fault};
        self.advance(index,self.clock_());
        const auto& timer=self.timers_[index];
        return {Stop::budget,field==0 ? timer.count :
            field==0x10 ? timer.mode : field==0x20 ? timer.target : timer.hold};
    }
    static Stop write(void* context,std::uint32_t address,std::uint32_t value) {
        auto& self=*static_cast<NativeTimers*>(context); unsigned index,field;
        if (!decode(address,index,field)) return Stop::memory_fault;
        auto& timer=self.timers_[index];
        if (field==0x10) {
            // Video gating and reset-on-compare are separate unsupported modes.
            if ((value&0x44) || (value&3)==3 || ((value&0x300) && !self.events_connected_)) return Stop::unsupported_system;
            self.advance(index,self.clock_());
            timer.mode=(timer.mode&0xc00&~value)|(value&0x3ff);
            timer.fraction=0; return Stop::budget;
        }
        self.advance(index,self.clock_());
        if (field==0) { timer.count=value&65535; timer.fraction=0; }
        else if (field==0x20) timer.target=value&65535;
        else timer.hold=value&65535;
        return Stop::budget;
    }
    Clock clock_;
    bool events_connected_=false;
    std::uint32_t events_=0;
    std::array<Timer,4> timers_{};
};
}
