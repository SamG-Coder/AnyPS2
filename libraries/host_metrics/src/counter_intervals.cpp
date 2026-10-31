#include <nt/counter_intervals.hpp>
#include <limits>
#include <stdexcept>
#include <utility>

namespace nt::host {
CounterIntervals::CounterIntervals(std::uint32_t thread,unsigned processors,unsigned counters)
    : thread_(thread),processors_(processors) {
    if (!thread || !processors || processors>4096 || !counters || counters>16)
        throw std::invalid_argument("Invalid native thread counter configuration");
    totals_.resize(counters); baseline_.resize(counters);
}
void CounterIntervals::fail(CounterStatus status) { if (!failure_) failure_=status; }
void CounterIntervals::lost_events() { fail(CounterStatus::lost_events); }
void CounterIntervals::consume(const SwitchSample& sample) {
    if (failure_) return;
    if (sample.processor>=processors_ || sample.counters.size()!=totals_.size()) {
        fail(CounterStatus::invalid_sample); return;
    }
    // Windows can report a scheduling boundary with the same outgoing and
    // incoming thread. Close and reopen its measured interval normally; this
    // neither drops the boundary nor counts its preceding work twice.
    if (timestamp_ && sample.timestamp<*timestamp_) { fail(CounterStatus::discontinuity); return; }
    timestamp_=sample.timestamp;
    if (active_ && *active_==sample.processor && sample.outgoing_thread!=thread_) {
        fail(CounterStatus::discontinuity); return;
    }
    if (sample.outgoing_thread==thread_) {
        if (!active_) {
            // Collection can begin midway through the target's first interval.
            // Discard that partial interval; never invent its starting reading.
            if (armed_) { fail(CounterStatus::discontinuity); return; }
        } else {
            if (*active_!=sample.processor) { fail(CounterStatus::discontinuity); return; }
            auto next=totals_;
            for (unsigned index=0;index<next.size();++index) {
                if (sample.counters[index]<baseline_[index]) { fail(CounterStatus::regressed_counter); return; }
                const auto delta=sample.counters[index]-baseline_[index];
                if (delta>std::numeric_limits<std::uint64_t>::max()-next[index]) { fail(CounterStatus::overflow); return; }
                next[index]+=delta;
            }
            if (intervals_==std::numeric_limits<std::uint64_t>::max()) { fail(CounterStatus::overflow); return; }
            totals_=std::move(next); ++intervals_; end_=sample.timestamp; active_.reset();
        }
    }
    if (sample.incoming_thread==thread_) {
        if (active_) { fail(CounterStatus::discontinuity); return; }
        baseline_.assign(sample.counters.begin(),sample.counters.end());
        active_=sample.processor; armed_=true;
    }
}
CounterSnapshot CounterIntervals::snapshot() const {
    const auto status=failure_ ? *failure_ : active_ ? CounterStatus::running :
        intervals_ ? CounterStatus::ready : CounterStatus::waiting;
    return {status,intervals_,end_,totals_};
}
}
