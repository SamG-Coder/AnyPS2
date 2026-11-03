#pragma once
#include <array>
#include <cstdint>

namespace nt::vu {
// Native function ABI. Instruction words are never stored or decoded here.
enum class Result : std::uint32_t { complete, unsupported_numeric, transfer, unsupported_schedule, unsupported_memory, unsupported_context, graphics_request };
struct State {
    std::array<std::array<std::uint32_t,4>,31> vectors{};
    std::array<std::uint16_t,15> integers{};
    std::uint32_t immediate=0;
    std::array<std::uint32_t,4> accumulator{};
    std::uint32_t mac=0,status=0;
    std::uint32_t next_pair=0;
    std::uint16_t delayed_integer=0,delayed_value=0;
    std::uint32_t quotient=0,pending_quotient=0,quotient_flags=0,quotient_pending=0;
    std::array<std::array<std::uint32_t,4>,1024> data{};
    std::uint16_t input_top=0,input_itop=0;
    std::uint32_t input_valid=0; // bit 0: TOP supplied; bit 1: ITOP supplied.
    std::uint32_t scalar_p=0,pending_p=0,p_pending=0;
    std::uint32_t mac_read_delay=0;
    std::uint32_t graphics_address=0,graphics_pending=0,graphics_completes=0;
    std::uint32_t clip=0,clip_read_delay=0;
    std::uint32_t random=0x3f800000;
    std::uint32_t quotient_read_delay=0;
    bool operator==(const State&) const = default;
};
}
