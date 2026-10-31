#pragma once
#include <nt/ee_native.hpp>
#include <functional>
#include <optional>

namespace nt::ee {
inline bool performance_active(std::uint32_t control,unsigned index) {
    const auto event=(control>>(5+10*index))&31U;
    return (control&0x80000000U) && (control&(0x1eU<<(10*index))) &&
        event!=16 && (index || event);
}

// Explicit native profiling policy, not an R5900 cycle model. Measures all CPU
// work on the owner thread, including native services. No elapsed-time scaling.
class NativeThreadCycles {
public:
    using Sample=std::function<std::optional<std::uint64_t>()>;
    enum class Error { none, unavailable_event, sample_failed, regressed_sample };
    explicit NativeThreadCycles(Sample sample) : sample_(std::move(sample)) {}
    NativeThreadCycles(const NativeThreadCycles&)=delete;
    NativeThreadCycles& operator=(const NativeThreadCycles&)=delete;
    void attach(Memory& memory) { memory.metrics={this,request}; }
    Error error() const { return error_; }
    std::uint64_t samples() const { return samples_; }
private:
    Sample sample_;
    std::optional<std::uint64_t> previous_;
    std::uint8_t unavailable_=0;
    Error error_=Error::none;
    std::uint64_t samples_=0;
    static bool measurable(std::uint32_t control,unsigned index) {
        // Mode-selective counting needs instrumentation at every transition;
        // a read's current mode cannot attribute the preceding interval.
        return performance_active(control,index) &&
            ((control>>(5+10*index))&31U)==1 &&
            ((control>>(10*index))&0x1eU)==0x1eU;
    }
    static IoRead request(void* raw,PerformanceState& state,PerformanceOperation op,
                          unsigned index,std::uint32_t value,std::uint32_t) {
        auto& self=*static_cast<NativeThreadCycles*>(raw);
        self.error_=Error::none;
        auto unknown=self.unavailable_;
        const auto next_control=op==PerformanceOperation::write_control ? value : state.control;
        bool needs_sample=false;
        for (unsigned i=0;i<2;++i) {
            if (performance_active(state.control,i) && (!measurable(state.control,i) || !self.previous_))
                unknown|=1U<<i;
            needs_sample|=measurable(state.control,i) || measurable(next_control,i);
        }
        auto now=self.previous_;
        if (needs_sample) {
            now=self.sample_ ? self.sample_() : std::nullopt;
            if (!now || (self.previous_ && *now<*self.previous_)) {
                self.error_=now ? Error::regressed_sample : Error::sample_failed;
                return {Stop::unsupported_system};
            }
            for (unsigned i=0;i<2;++i)
                if (measurable(state.control,i) && self.previous_)
                    state.counters[i]+=static_cast<std::uint32_t>(*now-*self.previous_);
        }
        IoRead result{Stop::budget};
        switch (op) {
        case PerformanceOperation::read_control: result.value=state.control; break;
        case PerformanceOperation::write_control: state.control=value; break;
        case PerformanceOperation::write_counter:
            state.counters[index]=value; unknown&=static_cast<std::uint8_t>(~(1U<<index)); break;
        case PerformanceOperation::read_counter:
            if (unknown&(1U<<index)) {
                self.error_=Error::unavailable_event;
                return {Stop::unsupported_system};
            }
            result.value=state.counters[index]; break;
        }
        for (unsigned i=0;i<2;++i)
            if (performance_active(state.control,i) && !measurable(state.control,i)) unknown|=1U<<i;
        self.unavailable_=unknown;
        self.previous_=now;
        if (needs_sample) ++self.samples_;
        return result;
    }
};
}
