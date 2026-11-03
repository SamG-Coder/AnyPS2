#pragma once
#include <cstdint>
#include <functional>
#include <span>

namespace nt {
// Complete, bounded requests from compiled peripheral drivers to native services.
struct NativePortExchange {
    unsigned port;
    std::span<const std::uint8_t> request;
    std::span<std::uint8_t> response;
    bool acknowledged=false;
};
using NativePortHandler=std::function<bool(std::span<NativePortExchange>)>;
}
