#pragma once
#include <nt/ee_native.hpp>
#include <cstring>
#include <span>
#include <vector>

namespace nt::ee {
struct NativePacketOrigin { std::size_t offset; unsigned quadword_offset; };
// Resolve data descriptors into an owned native batch. This does not execute VIF,
// GIF or VU code, clock a channel, or publish completion.
class NativePacketBatch {
public:
    std::vector<std::uint8_t> bytes;
    std::vector<NativePacketOrigin> origins;
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
    using ReadView=Stop (*)(const Memory&,std::uint32_t,std::uint32_t,std::span<const std::uint8_t>&);
    Stop chain(const Memory& memory,std::uint32_t first,bool transfer_tags,bool interrupt_tags,
               unsigned tag_bytes=8,ReadView read=view) {
        if ((tag_bytes!=8 && tag_bytes!=16) || !read) return Stop::invalid_state;
        tag=first;
        for (unsigned count=0;count<256;++count) {
            std::span<const std::uint8_t> descriptor;
            auto result=read(memory,tag,16,descriptor);
            if (result!=Stop::budget) return result;
            std::uint32_t words[2]; std::memcpy(words,descriptor.data(),sizeof(words));
            const auto kind=(words[0]>>28)&7U,size=(words[0]&0xffffU)*16U;
            const auto after=tag+16;
            tag_bits=words[0]&0xffff0000U;
            std::uint32_t next=tag;
            switch (kind) {
            // REFS has REF semantics when stall control is disabled. The
            // native submission policy rejects enabling console stall control.
            case 0: case 3: case 4: source=words[1]; next=after; break;
            case 1: source=after; next=after+size; break;
            case 2: source=after; next=words[1]; break;
            case 7: source=after; break;
            default: return Stop::unsupported_system; // Call and return need separate ownership.
            }
            std::span<const std::uint8_t> payload;
            result=read(memory,source,size,payload);
            if (result!=Stop::budget) return result;
            if (size+(transfer_tags ? tag_bytes : 0U)>maximum_bytes-bytes.size()) return Stop::invalid_state;
            if (transfer_tags) {
                origins.push_back({bytes.size(),16-tag_bytes});
                bytes.insert(bytes.end(),descriptor.end()-tag_bytes,descriptor.end());
            }
            if (!payload.empty()) {
                origins.push_back({bytes.size(),0});
                bytes.insert(bytes.end(),payload.begin(),payload.end());
            }
            source+=size; tag=next;
            if (kind==0 || kind==7 || (interrupt_tags && (words[0]&0x80000000U))) return Stop::budget;
        }
        return Stop::invalid_state;
    }
private:
    static constexpr std::size_t maximum_bytes=0x100000;
};
}
