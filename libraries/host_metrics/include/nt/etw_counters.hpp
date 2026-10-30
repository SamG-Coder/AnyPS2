#pragma once
#include <nt/counter_intervals.hpp>

struct _EVENT_RECORD;
namespace nt::host {
struct OwnedSwitchSample {
    unsigned processor;
    std::uint32_t outgoing_thread,incoming_thread;
    std::uint64_t timestamp;
    std::vector<std::uint64_t> counters;
    SwitchSample view() const { return {processor,outgoing_thread,incoming_thread,timestamp,counters}; }
};
// Input pointers must refer to a live Windows event callback record. Copies all
// retained data. Unrelated events return no sample; malformed/unsupported switch
// records throw and must invalidate the caller's measurement session.
std::optional<OwnedSwitchSample> decode_counter_switch(const _EVENT_RECORD& record,unsigned counter_count);
}
