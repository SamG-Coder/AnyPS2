#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace nt::graphics {
using Quadword=std::array<std::uint32_t,4>;
// Frame data packets for a native consumer. This is not an instruction decoder.
// Bound extraction to one traversal of the source ring; oversized packets stop.
inline std::vector<Quadword> collect_packet(std::span<const Quadword> ring,unsigned start) {
    if (ring.empty() || start>=ring.size()) throw std::invalid_argument("Invalid graphics packet source");
    std::vector<Quadword> packet;
    std::size_t consumed=0;
    while (consumed<ring.size()) {
        const auto& tag=ring[(start+consumed)%ring.size()];
        const auto low=std::uint64_t(tag[0])|(std::uint64_t(tag[1])<<32);
        const auto loops=low&0x7fffU,format=(low>>58)&3U;
        const auto encoded_registers=low>>60;
        const auto registers=encoded_registers ? encoded_registers : 16U;
        const auto entries=loops*registers;
        const auto payload=format==0 ? entries : format==1 ? (entries+1)/2 : loops;
        if (payload>ring.size()-consumed-1) throw std::invalid_argument("Graphics packet exceeds source ring bound");
        for (std::size_t i=0;i<=payload;++i) packet.push_back(ring[(start+consumed+i)%ring.size()]);
        consumed+=static_cast<std::size_t>(payload)+1;
        if (low&0x8000U) return packet;
    }
    throw std::invalid_argument("Graphics packet has no end tag within bound");
}
}
