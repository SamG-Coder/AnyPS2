#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace nt::host {
// Owns a private ETW session; samples complete on-CPU intervals of the creating
// thread. Samples are the latest delivered complete intervals, not synchronous
// hardware reads; an active interval is never extrapolated. Only the first
// sample can wait to establish a measured baseline.
class LiveCounters {
public:
    LiveCounters();
    ~LiveCounters();
    LiveCounters(const LiveCounters&)=delete;
    LiveCounters& operator=(const LiveCounters&)=delete;
    std::optional<std::array<std::uint64_t,2>> sample();
    std::string error() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
