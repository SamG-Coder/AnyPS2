#pragma once
#include <nt/ee_native.hpp>
#include <functional>
#include <optional>
#include <array>

namespace nt::ee {
inline bool performance_active(std::uint32_t control,unsigned index) {
    const auto event=(control>>(5+10*index))&31U;
    return (control&0x80000000U) && (control&(0x1eU<<(10*index))) &&
        event!=16 && (index || event);
}

// Explicit native profiling policies, not an R5900 counter model. Elapsed-time
// policy replaces cycle/cache profiling with clock ticks, without CPU scaling.
class NativePerformanceCounters {
public:
    using Values=std::array<std::uint64_t,2>;
    using Sample=std::function<std::optional<Values>()>;
    using CycleSample=std::function<std::optional<std::uint64_t>()>;
    enum class ClockPolicy { cpu_cycles, elapsed_time };
    enum class Error { none, unavailable_event, sample_failed, regressed_sample };
    explicit NativePerformanceCounters(Sample sample,bool cache) : sample_(std::move(sample)),cache_(cache) {}
    explicit NativePerformanceCounters(CycleSample sample,ClockPolicy policy=ClockPolicy::cpu_cycles) : sample_([sample=std::move(sample)]() -> std::optional<Values> {
        const auto value=sample(); if (!value) return {}; return Values{*value,0};
    }),elapsed_(policy==ClockPolicy::elapsed_time) {}
    NativePerformanceCounters(const NativePerformanceCounters&)=delete;
    NativePerformanceCounters& operator=(const NativePerformanceCounters&)=delete;
    void attach(Memory& memory) { memory.metrics={this,request}; }
    Error error() const { return error_; }
    std::uint64_t samples() const { return samples_; }
    std::optional<Values> last_sample() const { return previous_; }
private:
    Sample sample_;
    std::optional<Values> previous_;
    bool cache_=false;
    bool elapsed_=false;
    std::uint8_t unavailable_=0;
    Error error_=Error::none;
    std::uint64_t samples_=0;
    bool measurable(std::uint32_t control,unsigned index) const {
        // Mode-selective counting needs instrumentation at every transition;
        // a read's current mode cannot attribute the preceding interval.
        return performance_active(control,index) &&
            (((control>>(5+10*index))&31U)==1 ||
             ((cache_ || elapsed_) && index==1 && ((control>>15)&31U)==6)) &&
            ((control>>(10*index))&0x1eU)==0x1eU;
    }
    static IoRead request(void* raw,PerformanceState& state,PerformanceOperation op,
                          unsigned index,std::uint32_t value,std::uint32_t) {
        auto& self=*static_cast<NativePerformanceCounters*>(raw);
        self.error_=Error::none;
        auto unknown=self.unavailable_;
        const auto next_control=op==PerformanceOperation::write_control ? value : state.control;
        const bool clock_read=op==PerformanceOperation::read_clock;
        if (clock_read && !self.elapsed_) {
            self.error_=Error::unavailable_event; return {Stop::unsupported_system};
        }
        bool needs_sample=clock_read;
        for (unsigned i=0;i<2;++i) {
            if (performance_active(state.control,i) && (!self.measurable(state.control,i) || !self.previous_))
                unknown|=1U<<i;
            needs_sample|=self.measurable(state.control,i) || self.measurable(next_control,i);
        }
        auto now=self.previous_;
        if (needs_sample) {
            now=self.sample_ ? self.sample_() : std::nullopt;
            if (!now || (self.previous_ && ((*now)[0]<(*self.previous_)[0] || (*now)[1]<(*self.previous_)[1]))) {
                self.error_=now ? Error::regressed_sample : Error::sample_failed;
                return {Stop::unsupported_system};
            }
            for (unsigned i=0;i<2;++i)
                if (self.measurable(state.control,i) && self.previous_) {
                    const unsigned source=self.elapsed_ || ((state.control>>(5+10*i))&31U)==1 ? 0 : 1;
                    state.counters[i]+=static_cast<std::uint32_t>((*now)[source]-(*self.previous_)[source]);
                }
        }
        IoRead result{Stop::budget};
        switch (op) {
        case PerformanceOperation::read_clock: result.value=static_cast<std::uint32_t>((*now)[0]); break;
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
            if (performance_active(state.control,i) && !self.measurable(state.control,i)) unknown|=1U<<i;
        self.unavailable_=unknown;
        self.previous_=now;
        if (needs_sample) ++self.samples_;
        return result;
    }
};
}
