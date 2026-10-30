#pragma once
#include <nt/ee_native.hpp>
#include <cstring>
#include <span>
#include <vector>

namespace nt::ee {
// Resolve data descriptors into an owned native batch. This does not execute VIF,
// GIF or VU code, clock a channel, or publish completion.
class NativePacketBatch {
public:
    std::vector<std::uint8_t> bytes;
    std::uint32_t source=0,tag=0,tag_bits=0;
    static Stop view(const Memory& memory,std::uint32_t raw,std::uint32_t size,
                     std::span<const std::uint8_t>& result) {
        const bool scratch=(raw&0x80000000U)!=0;
        const auto offset=scratch ? raw&0x3fffU : runtime::default_ram_address(raw);
        const auto* data=scratch ? memory.scratchpad : memory.data;
        const auto available=scratch ? memory.scratchpad_bytes : memory.size;
        const auto limit=scratch ? scratchpad_size : 0x2000000U;
        if (raw%16 || (scratch && (raw&0x7fffc000U)) || !data ||
            std::uint64_t(offset)+size>available || std::uint64_t(offset)+size>limit)
            return Stop::memory_fault;
        result={data+offset,size}; return Stop::budget;
    }
    Stop chain(const Memory& memory,std::uint32_t first,bool transfer_tags,bool interrupt_tags) {
        tag=first;
        for (unsigned count=0;count<256;++count) {
            std::span<const std::uint8_t> descriptor;
            auto result=view(memory,tag,16,descriptor);
            if (result!=Stop::budget) return result;
            std::uint32_t words[2]; std::memcpy(words,descriptor.data(),sizeof(words));
            const auto kind=(words[0]>>28)&7U,size=(words[0]&0xffffU)*16U;
            const auto after=tag+16;
            tag_bits=words[0]&0xffff0000U;
            std::uint32_t next=tag;
            switch (kind) {
            case 0: case 3: source=words[1]; next=after; break;
            case 1: source=after; next=after+size; break;
            case 2: source=after; next=words[1]; break;
            case 7: source=after; break;
            default: return Stop::unsupported_system; // Stall, call and return need separate ownership.
            }
            std::span<const std::uint8_t> payload;
            result=view(memory,source,size,payload);
            if (result!=Stop::budget) return result;
            if (size+(transfer_tags ? 8U : 0U)>maximum_bytes-bytes.size()) return Stop::invalid_state;
            if (transfer_tags) bytes.insert(bytes.end(),descriptor.begin()+8,descriptor.end());
            bytes.insert(bytes.end(),payload.begin(),payload.end());
            source+=size; tag=next;
            if (kind==0 || kind==7 || (interrupt_tags && (words[0]&0x80000000U))) return Stop::budget;
        }
        return Stop::invalid_state;
    }
private:
    static constexpr std::size_t maximum_bytes=0x100000;
};
}
