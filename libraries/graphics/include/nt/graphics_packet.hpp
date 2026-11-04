#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>
#include <utility>
#include <bit>
#include <cstring>

namespace nt::graphics {
using Quadword=std::array<std::uint32_t,4>;
using Packet=std::vector<Quadword>;
// Aggregate native submission bound; individual packet framing is bounded
// separately. A compiled VU batch may contain many complete packets.
inline constexpr std::size_t maximum_batch_bytes=32U*1024U*1024U;
namespace detail {
// Native transport bytes are little endian and may be unaligned. Copy payloads
// in bulk on little-endian hosts; retain explicit decoding on other hosts.
inline void append_quadwords(Packet& words,std::span<const std::uint8_t> bytes) {
    if (bytes.size()%16) throw std::invalid_argument("Unaligned graphics payload size");
    const auto first=words.size();
    words.resize(first+bytes.size()/16);
    if constexpr (std::endian::native==std::endian::little) {
        if (!bytes.empty()) std::memcpy(words.data()+first,bytes.data(),bytes.size());
    } else {
        for (std::size_t i=0;i<bytes.size();++i)
            words[first+i/16][(i%16)/4]|=std::uint32_t(bytes[i])<<((i%4)*8);
    }
}
}
struct BatchConsumer {
    void* context=nullptr;
    // Accept the complete ordered batch or leave external state unchanged.
    bool (*submit)(void*,std::span<const Packet>)=nullptr;
    // Drain previously accepted commands into the ordered native graphics queue.
    // This is command consumption, not raster completion; FINISH/readback retain
    // real GPU fences. Failure must not publish a new batch or alter configuration.
    bool (*wait)(void*)=nullptr;
    // True when submit consumes every command into that ordered queue before
    // returning. Allows a trailing command-drain within an atomic batch.
    bool synchronous=false;
};
inline std::size_t payload_words(const Quadword& tag) {
    const auto low=std::uint64_t(tag[0])|(std::uint64_t(tag[1])<<32);
    const auto loops=low&0x7fffU,format=(low>>58)&3U;
    const auto encoded=low>>60,registers=encoded ? encoded : 16U;
    const auto entries=loops*registers;
    return static_cast<std::size_t>(format==0 ? entries : format==1 ? (entries+1)/2 : loops);
}
// Incremental transport framing. The caller works on a copy and commits it only
// after the complete output batch is accepted by the native drawing consumer.
class PacketStream {
public:
    bool pending() const { return !packet_.empty(); }
    void append(std::span<const std::uint8_t> bytes,std::vector<Packet>& output) {
        constexpr std::size_t limit=65536;
        if (bytes.empty() || bytes.size()%16 || bytes.size()>maximum_batch_bytes)
            throw std::invalid_argument("Invalid graphics stream extent");
        std::size_t offset=0;
        while (offset<bytes.size()) {
            if (!remaining_) {
                // Only tags need inspection; payload words are opaque data.
                if (packet_.size()==limit)
                    throw std::invalid_argument("Graphics stream packet exceeds bound");
                detail::append_quadwords(packet_,bytes.subspan(offset,16)); offset+=16;
                const auto& word=packet_.back();
                remaining_=payload_words(word);
                if (remaining_>limit-packet_.size())
                    throw std::invalid_argument("Graphics stream packet exceeds bound");
                end_=(word[0]&0x8000U)!=0;
            } else {
                const auto count=std::min(remaining_,(bytes.size()-offset)/16);
                detail::append_quadwords(packet_,bytes.subspan(offset,count*16));
                remaining_-=count; offset+=count*16;
            }
            if (!remaining_ && end_) {
                output.push_back(std::move(packet_)); packet_.clear(); end_=false;
            }
        }
    }
private:
    Packet packet_;
    std::size_t remaining_=0;
    bool end_=false;
};
// Frame data packets for a native consumer. This is not an instruction decoder.
// Bound extraction to one traversal of the source ring; oversized packets stop.
inline std::vector<Quadword> collect_packet(std::span<const Quadword> ring,unsigned start) {
    if (ring.empty() || start>=ring.size()) throw std::invalid_argument("Invalid graphics packet source");
    std::vector<Quadword> packet;
    std::size_t consumed=0;
    while (consumed<ring.size()) {
        const auto& tag=ring[(start+consumed)%ring.size()];
        const auto low=std::uint64_t(tag[0])|(std::uint64_t(tag[1])<<32);
        const auto payload=payload_words(tag);
        if (payload>ring.size()-consumed-1) throw std::invalid_argument("Graphics packet exceeds source ring bound");
        for (std::size_t i=0;i<=payload;++i) packet.push_back(ring[(start+consumed+i)%ring.size()]);
        consumed+=static_cast<std::size_t>(payload)+1;
        if (low&0x8000U) return packet;
    }
    throw std::invalid_argument("Graphics packet has no end tag within bound");
}
inline void append_packets(std::vector<Packet>& packets,std::span<const std::uint8_t> bytes) {
    PacketStream stream;
    stream.append(bytes,packets);
    if (stream.pending()) throw std::invalid_argument("Incomplete native graphics transfer");
}
}
