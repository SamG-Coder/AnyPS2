#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace nt::host {
enum class CounterStatus {
    waiting, running, ready, invalid_sample, lost_events, discontinuity,
    regressed_counter, overflow
};
struct SwitchSample {
    unsigned processor;
    std::uint32_t outgoing_thread,incoming_thread;
    std::uint64_t timestamp;
    std::span<const std::uint64_t> counters;
};
struct CounterSnapshot {
    CounterStatus status;
    std::uint64_t completed_intervals,end_timestamp;
    std::vector<std::uint64_t> totals;
};
// Serialized, globally timestamp-ordered context-switch samples only. Counters
// must be monotonic readings from the same configured sources on each CPU.
// Reports complete on-CPU intervals; never extrapolates an active interval.
class CounterIntervals {
public:
    CounterIntervals(std::uint32_t thread,unsigned processors,unsigned counters);
    void consume(const SwitchSample& sample);
    void lost_events();
    CounterSnapshot snapshot() const;
private:
    void fail(CounterStatus status);
    std::uint32_t thread_;
    unsigned processors_;
    std::vector<std::uint64_t> totals_,baseline_;
    std::optional<unsigned> active_;
    std::optional<std::uint64_t> timestamp_;
    std::uint64_t intervals_=0,end_=0;
    bool armed_=false;
    std::optional<CounterStatus> failure_;
};
}
