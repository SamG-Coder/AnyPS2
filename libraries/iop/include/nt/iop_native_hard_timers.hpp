#pragma once
#include <nt/iop_native_interrupts.hpp>
#include <chrono>
#include <bit>
#include <functional>

namespace nt::iop {
// Timer handles retain the public ABI, while elapsed time comes from a native
// monotonic clock. No timer MMIO bank or instruction-based clock is installed.
class NativeHardTimers final : public NativeServices, public NativeCallbackSource {
public:
    using Clock=std::function<std::uint64_t()>;
    explicit NativeHardTimers(Clock clock={}) : clock_(std::move(clock)) {
        if (!clock_) clock_=[epoch=std::chrono::steady_clock::now()] {
            return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-epoch).count());
        };
    }
    void connect(NativeInterruptServices& interrupts) { interrupts_=&interrupts; }
    NativeServiceReply invoke(const NativeServiceRequest& r,std::span<std::uint8_t> ram) override {
        if (r.kind!=NativeServiceKind::timrman ||
            !(r.selector==4 || (r.selector>=6 && r.selector<=12) || r.selector==16)) return {};
        const auto& a=r.arguments;
        const auto returned=[](std::int32_t value=0) { return NativeServiceReply{NativeServiceStatus::returned,static_cast<std::uint32_t>(value)}; };
        if (r.selector==4) {
            if (r.interrupt_context) return returned(-100);
            if (!a[0] || a[0]>15 || !a[2]) return returned(-150);
            for (const unsigned i : {2U,5U,4U,3U,0U,1U}) {
                const auto& spec=specs_[i];
                if (timers_[i].used || !(spec.sources&a[0]) || spec.bits!=a[1] || a[2]>spec.prescale) continue;
                timers_[i]=Timer{}; timers_[i].used=true; timers_[i].epoch=clock_();
                if (interrupts_) { interrupts_->invoke({7,{spec.irq,0},NativeServiceKind::intrman},ram); interrupts_->clear_pending(spec.irq); }
                return returned(static_cast<std::int32_t>(handle(i)));
            }
            return returned(-150);
        }
        unsigned index=6;
        for (unsigned i=0;i<6;++i) if (a[0]==handle(i) && timers_[i].used) index=i;
        if (index==6) return returned(-151);
        auto& timer=timers_[index];
        const auto& spec=specs_[index];
        if (r.selector==6) {
            if (r.interrupt_context) return returned(-100);
            if (interrupts_) { interrupts_->invoke({7,{spec.irq,0},NativeServiceKind::intrman},ram); interrupts_->clear_pending(spec.irq); }
            timer=Timer{}; return returned();
        }
        if (r.selector==16) return returned(static_cast<std::int32_t>(spec.irq));
        if (r.selector==7) {
            // Gates, external clocks and toggle interrupts need additional native
            // event sources. Reject them before changing existing configuration.
            if (a[1]&~0x6278U || (index<3 && (a[1]&0x6000)) || (index>=3 && (a[1]&0x200)))
                return {NativeServiceStatus::unavailable};
            timer.mode=a[1]; timer.count=0; timer.epoch=clock_(); timer.remainder=0; timer.fired=false; timer.flags=0;
            return returned();
        }
        update(index);
        switch (r.selector) {
        case 8: { const auto value=timer.mode|timer.flags; timer.flags=0; return returned(static_cast<std::int32_t>(value)); }
        case 9: timer.count=a[1]&mask(spec); timer.epoch=clock_(); timer.remainder=0; return returned();
        case 10: return returned(std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(timer.count)));
        case 11: timer.compare=a[1]&mask(spec); return returned();
        case 12: return returned(static_cast<std::int32_t>(timer.compare));
        default: return {};
        }
    }
    std::optional<NativeCallback> take() override {
        for (unsigned i=0;i<6;++i) if (timers_[i].used) update(i);
        return {};
    }
    void finish(std::uint32_t) override { throw std::logic_error("Timer events use the interrupt dispatcher"); }
private:
    struct Spec { std::uint32_t address,sources,bits,prescale,irq; };
    inline static constexpr std::array<Spec,6> specs_{{
        {0x1f801100,11,16,1,4},{0x1f801110,13,16,1,5},{0x1f801120,1,16,8,6},
        {0x1f801480,5,32,1,14},{0x1f801490,1,32,256,15},{0x1f8014a0,1,32,256,16}}};
    struct Timer { bool used=false,fired=false; std::uint32_t mode=0,compare=0,flags=0; std::uint64_t count=0,epoch=0,remainder=0; };
    static std::uint32_t handle(unsigned i) { return ((i+1)<<28)|(specs_[i].address>>4); }
    static std::uint32_t mask(const Spec& spec) { return spec.bits==16 ? 0xffffU : 0xffffffffU; }
    void update(unsigned i) {
        auto& t=timers_[i]; const auto& spec=specs_[i];
        const auto now=clock_(); if (now<=t.epoch) return;
        const auto ns=now-t.epoch; t.epoch=now;
        const auto divisor=i<3 ? ((t.mode&0x200) ? 8U : 1U) : std::array<unsigned,4>{1,8,16,256}[(t.mode>>13)&3];
        const std::uint64_t frequency=36864000/divisor;
        const auto fractional=(ns%1000000000)*frequency+t.remainder;
        const auto ticks=(ns/1000000000)*frequency+fractional/1000000000;
        t.remainder=fractional%1000000000;
        const auto modulus=std::uint64_t(mask(spec))+1;
        const auto period=(t.mode&8) && t.compare ? t.compare : modulus;
        const auto distance=t.compare>t.count ? t.compare-t.count : modulus-t.count+t.compare;
        const bool compare=ticks>=distance;
        const bool overflow=ticks>=modulus-t.count && (!(t.mode&8) || distance>=modulus-t.count);
        t.count=(t.mode&8) && compare ? (ticks-distance)%period : (t.count+ticks)%modulus;
        if (compare) t.flags|=0x800;
        if (overflow) t.flags|=0x1000;
        if (interrupts_ && (!t.fired || (t.mode&0x40)) && ((compare && (t.mode&0x10)) || (overflow && (t.mode&0x20)))) {
            interrupts_->post(spec.irq); t.fired=true;
        }
    }
    std::array<Timer,6> timers_{};
    Clock clock_;
    NativeInterruptServices* interrupts_=nullptr;
};
}
