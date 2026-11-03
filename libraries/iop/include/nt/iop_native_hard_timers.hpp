#pragma once
#include <nt/iop_native_interrupts.hpp>
#include <nt/iop_native_code.hpp>
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
            !(r.selector==4 || (r.selector>=6 && r.selector<=12) || r.selector==16 ||
              r.selector==20 || (r.selector>=22 && r.selector<=24))) return {};
        const auto& a=r.arguments;
        const auto returned=[](std::int32_t value=0) { return NativeServiceReply{NativeServiceStatus::returned,static_cast<std::uint32_t>(value)}; };
        if (r.selector==4) {
            if (r.interrupt_context) return returned(-100);
            if (!a[0] || a[0]>15 || !a[2]) return returned(-150);
            for (const unsigned i : {2U,5U,4U,3U,0U,1U}) {
                const auto& spec=specs_[i];
                if (timers_[i].used || !(spec.sources&a[0]) || spec.bits!=a[1] || a[2]>spec.prescale) continue;
                timers_[i]=Timer{}; timers_[i].used=true; timers_[i].epoch=clock_(); timers_[i].generation=++generation_;
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
        if (r.selector==20) {
            if (timer.running) return returned(-156);
            if (a[2]) {
                const auto target=ram_offset(a[2],4,ram.size());
                if (!target || !nt_iop_native_instruction(*target)) return {NativeServiceStatus::unmapped_code,0,a[2]};
            }
            timer.compare=a[1]&mask(spec); timer.callback.reset(); timer.pending=false;
            if (a[2]) timer.callback=NativeCallback{a[2],a[3],{},r.global_pointer};
            return returned();
        }
        if (r.selector==22) {
            if (r.interrupt_context) return returned(-100);
            if (timer.running) return returned(-154);
            // External pixel/scanline clocks and gates need a real event source.
            if (a[1]!=1 || a[2]!=0) return {NativeServiceStatus::unavailable};
            const auto divisor=a[3];
            if (!(divisor==1 || divisor==8 || divisor==16 || divisor==256) || divisor>spec.prescale)
                return returned(-152);
            timer.managed=true; timer.configured=true; timer.divisor=divisor;
            timer.mode=0; timer.pending=false; timer.count=0; timer.remainder=0; timer.epoch=clock_();
            return returned();
        }
        if (r.selector==23) {
            if (r.interrupt_context) return returned(-100);
            if (timer.running) return returned(-154);
            if (!timer.configured) return returned(-155);
            timer.running=true; timer.pending=false; timer.flags=0;
            timer.generation=++generation_;
            timer.count=0; timer.remainder=0; timer.epoch=clock_();
            return returned();
        }
        if (r.selector==24) {
            if (!timer.managed) return {NativeServiceStatus::unavailable};
            update(index); timer.running=false; timer.pending=false;
            return returned();
        }
        if (r.selector==7) {
            // Gates, external clocks and toggle interrupts need additional native
            // event sources. Reject them before changing existing configuration.
            if (a[1]&~0x6278U || (index<3 && (a[1]&0x6000)) || (index>=3 && (a[1]&0x200)))
                return {NativeServiceStatus::unavailable};
            if (timer.managed) return {NativeServiceStatus::unavailable};
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
        if (active_) return {};
        for (unsigned i=0;i<6;++i) {
            auto& timer=timers_[i];
            if (!timer.pending || !timer.callback) continue;
            timer.pending=false; active_=i; active_generation_=timer.generation;
            return timer.callback;
        }
        return {};
    }
    void finish(std::uint32_t result) override {
        if (!active_) throw std::logic_error("No active native timer callback");
        const auto index=*active_; auto& timer=timers_[index]; active_.reset();
        if (!timer.used || timer.generation!=active_generation_ || !timer.running) return;
        if (!result) { timer.running=false; timer.pending=false; }
        else timer.compare=result&mask(specs_[index]);
    }
private:
    struct Spec { std::uint32_t address,sources,bits,prescale,irq; };
    inline static constexpr std::array<Spec,6> specs_{{
        {0x1f801100,11,16,1,4},{0x1f801110,13,16,1,5},{0x1f801120,1,16,8,6},
        {0x1f801480,5,32,1,14},{0x1f801490,1,32,256,15},{0x1f8014a0,1,32,256,16}}};
    struct Timer {
        bool used=false,fired=false,managed=false,configured=false,running=false,pending=false;
        std::uint32_t mode=0,compare=0,flags=0,divisor=1;
        std::uint64_t count=0,epoch=0,remainder=0,generation=0;
        std::optional<NativeCallback> callback;
    };
    static std::uint32_t handle(unsigned i) { return ((i+1)<<28)|(specs_[i].address>>4); }
    static std::uint32_t mask(const Spec& spec) { return spec.bits==16 ? 0xffffU : 0xffffffffU; }
    void update(unsigned i) {
        auto& t=timers_[i]; const auto& spec=specs_[i];
        if (t.managed && !t.running) return;
        const auto now=clock_(); if (now<=t.epoch) return;
        const auto ns=now-t.epoch; t.epoch=now;
        const auto divisor=t.managed ? t.divisor : i<3 ? ((t.mode&0x200) ? 8U : 1U) : std::array<unsigned,4>{1,8,16,256}[(t.mode>>13)&3];
        const std::uint64_t frequency=36864000/divisor;
        const auto fractional=(ns%1000000000)*frequency+t.remainder;
        const auto ticks=(ns/1000000000)*frequency+fractional/1000000000;
        t.remainder=fractional%1000000000;
        const auto modulus=std::uint64_t(mask(spec))+1;
        const bool reset=t.managed ? t.callback.has_value() : (t.mode&8)!=0;
        const auto period=reset && t.compare ? t.compare : modulus;
        const auto distance=t.compare>t.count ? t.compare-t.count : modulus-t.count+t.compare;
        const bool compare=ticks>=distance;
        const bool overflow=ticks>=modulus-t.count && (!reset || distance>=modulus-t.count);
        t.count=reset && compare ? (ticks-distance)%period : (t.count+ticks)%modulus;
        if (compare) t.flags|=0x800;
        if (overflow) t.flags|=0x1000;
        if (t.managed) { if (compare && t.callback) t.pending=true; return; }
        if (interrupts_ && (!t.fired || (t.mode&0x40)) && ((compare && (t.mode&0x10)) || (overflow && (t.mode&0x20)))) {
            interrupts_->post(spec.irq); t.fired=true;
        }
    }
    std::array<Timer,6> timers_{};
    Clock clock_;
    NativeInterruptServices* interrupts_=nullptr;
    std::optional<unsigned> active_;
    std::uint64_t generation_=0,active_generation_=0;
};
}
