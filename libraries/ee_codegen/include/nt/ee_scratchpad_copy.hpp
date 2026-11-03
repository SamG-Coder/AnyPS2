#pragma once
#include <nt/ee_packet_batch.hpp>
#include <nt/ee_submission_policy.hpp>
#include <nt/ee_completions.hpp>
#include <nt/ee_handlers.hpp>
#include <algorithm>
#include <array>

namespace nt::ee {
// Synchronous native buffer copies. Descriptor parsing prepares a bounded
// transaction; it neither executes instructions nor simulates channel timing.
class NativeScratchpadCopy {
public:
    CodeOverlap protection=nullptr;
    static bool maps(std::uint32_t address) {
        return (address>=0x1000d000 && address<=0x1000d080) ||
               (address>=0x1000d400 && address<=0x1000d480);
    }
    IoRead read(std::uint32_t address) const {
        unsigned channel,field;
        if (!decode(address,channel,field)) return {Stop::unsupported_system};
        return {Stop::budget,configuration_[channel][field]};
    }
    Stop write(Memory& memory,const NativeSubmissionPolicy& policy,std::uint32_t address,std::uint32_t value) {
        unsigned channel,field;
        if (!decode(address,channel,field)) return Stop::unsupported_system;
        auto& config=configuration_[channel];
        if (field) {
            config[field]=field==2 ? value&0xffffU : field==8 ? value&0x3ff0U : value;
            return Stop::budget;
        }
        if (!(value&0x100)) { config[0]=value; return Stop::budget; }
        const auto mode=value&0xc;
        if (!policy.enabled() || !memory.completions || (value&0xfe32U) ||
            (mode!=0 && mode!=4)) return Stop::unsupported_system;
        if (!memory.scratchpad || memory.scratchpad_bytes<scratchpad_size) return Stop::memory_fault;
        auto next=config; next[0]=value;
        const auto result=channel ? to_scratch(memory,next,mode) : from_scratch(memory,next,mode);
        if (result!=Stop::budget) return result;
        next[0]&=~0x100U; next[2]=0; config=next;
        memory.completions->complete(8+channel); memory.completions->publish(memory);
        return Stop::budget;
    }
private:
    using Config=std::array<std::uint32_t,9>;
    std::array<Config,2> configuration_{};
    static bool decode(std::uint32_t address,unsigned& channel,unsigned& field) {
        if (!maps(address) || address%16) return false;
        channel=(address-0x1000d000)/0x400; field=(address&0xff)/16;
        return field<=5 || field==8;
    }
    static Stop source_view(const Memory& memory,std::uint32_t raw,std::uint32_t size,
                            std::span<const std::uint8_t>& result) {
        if (raw%16) return Stop::memory_fault;
        if ((raw&0x70000000U)==0x70000000U) return Stop::unsupported_system; // Aliased self-copy needs ordered overlap semantics.
        const auto address=raw&0x1fffffffU;
        if (address>=0x11000000 && address<0x11010000) return Stop::unsupported_vector;
        if (!memory.data || std::uint64_t(address)+size>memory.size || std::uint64_t(address)+size>0x2000000U)
            return Stop::memory_fault;
        result={memory.data+address,size}; return Stop::budget;
    }
    static void scratch_copy(Memory& memory,std::uint32_t& offset,std::span<std::uint8_t> bytes,bool to_scratch) {
        while (!bytes.empty()) {
            const auto size=std::min<std::size_t>(bytes.size(),scratchpad_size-offset);
            if (to_scratch) std::memcpy(memory.scratchpad+offset,bytes.data(),size);
            else std::memcpy(bytes.data(),memory.scratchpad+offset,size);
            offset=(offset+static_cast<std::uint32_t>(size))&(scratchpad_size-1);
            bytes=bytes.subspan(size);
        }
    }
    static Stop to_scratch(Memory& memory,Config& next,unsigned mode) {
        NativePacketBatch batch;
        if (mode==4) {
            if (next[2]) return Stop::unsupported_system;
            const auto status=batch.chain(memory,next[3],(next[0]&0x40)!=0,(next[0]&0x80)!=0,16,source_view);
            if (status!=Stop::budget) return status;
            next[1]=batch.source; next[3]=batch.tag;
            next[0]=(next[0]&0xffffU)|batch.tag_bits;
        } else {
            std::span<const std::uint8_t> source;
            const auto status=source_view(memory,next[1],next[2]*16,source);
            if (status!=Stop::budget) return status;
            batch.bytes.assign(source.begin(),source.end()); next[1]+=next[2]*16;
        }
        scratch_copy(memory,next[8],batch.bytes,true);
        return Stop::budget;
    }
    Stop from_scratch(Memory& memory,Config& next,unsigned mode) const {
        struct Write { std::uint32_t destination; std::vector<std::uint8_t> bytes; };
        std::vector<Write> writes;
        const auto append=[&](std::uint32_t raw,std::uint32_t size)->Stop {
            std::span<const std::uint8_t> destination;
            const auto valid=source_view(memory,raw,size,destination);
            if (valid!=Stop::budget) return valid;
            if (!protection) return Stop::invalid_state;
            const auto physical=raw&0x1fffffffU;
            if (size && protection(physical,size)) { memory.fault_address=raw; return Stop::code_write; }
            writes.push_back({physical,std::vector<std::uint8_t>(size)});
            scratch_copy(memory,next[8],writes.back().bytes,false);
            next[1]=raw+size; return Stop::budget;
        };
        if (!mode) {
            const auto status=append(next[1],next[2]*16);
            if (status!=Stop::budget) return status;
        } else {
            if (next[2]) return Stop::unsupported_system;
            std::size_t total=0;
            bool ended=false;
            for (unsigned count=0;count<256;++count) {
                std::array<std::uint8_t,16> descriptor;
                scratch_copy(memory,next[8],descriptor,false);
                std::uint32_t words[2]; std::memcpy(words,descriptor.data(),sizeof(words));
                const auto kind=(words[0]>>28)&7U,size=(words[0]&0xffffU)*16;
                if (kind!=0 && kind!=1 && kind!=7) return Stop::unsupported_system;
                if (size>0x100000-total) return Stop::invalid_state;
                total+=size;
                const auto status=append(words[1],size);
                if (status!=Stop::budget) return status;
                next[0]=(next[0]&0xffffU)|(words[0]&0xffff0000U);
                if (kind==7 || ((next[0]&0x80) && (words[0]&0x80000000U))) { ended=true; break; }
            }
            if (!ended) return Stop::invalid_state;
        }
        for (const auto& write:writes)
            if (!write.bytes.empty()) std::memcpy(memory.data+write.destination,write.bytes.data(),write.bytes.size());
        return Stop::budget;
    }
};
}
