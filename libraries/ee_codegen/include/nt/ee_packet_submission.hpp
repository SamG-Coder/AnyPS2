#pragma once
#include <nt/ee_completions.hpp>
#include <nt/ee_submission_policy.hpp>
#include <nt/ee_packet_batch.hpp>
#include <array>
#include <span>

namespace nt::ee {
// A synchronous native packet consumer. Success means all bytes were consumed;
// every other result must leave consumer state unchanged. The view is borrowed
// only for this call. Descriptor batches have no transfer clock.
struct NativePacketConsumer {
    void* context=nullptr;
    Stop (*consume)(void*,unsigned,std::span<const std::uint8_t>)=nullptr;
};
class NativePacketSubmission {
public:
    NativePacketConsumer consumer;
    NativeSubmissionPolicy policy;
    static bool maps(std::uint32_t address) { return address>=0x10008000 && address<0x1000b000; }
    IoRead read(std::uint32_t address) const {
        unsigned channel,field;
        if (!decode(address,channel,field)) return {Stop::unsupported_system};
        return {Stop::budget,channels_[channel][field]};
    }
    Stop write(Memory& memory,std::uint32_t address,std::uint32_t value) {
        unsigned channel,field;
        if (!decode(address,channel,field)) return Stop::unsupported_system;
        auto& config=channels_[channel];
        if (field==8) return value ? Stop::unsupported_system : Stop::budget;
        if (field) {
            config[field]=field==2 ? value&0xffffU : value;
            return Stop::budget;
        }
        // Idle configuration is metadata. Starting work requires a native
        // consumer, normal forward transfer and a complete bounded source.
        if (!(value&0x100)) { config[0]=value; return Stop::budget; }
        const auto mode=value&0xc;
        if (!policy.enabled() || !(value&1) || (mode!=0 && mode!=4) ||
            (value&0x0000fe32U) || (channel==2 && (value&0x40)) || !consumer.consume || !memory.completions)
            return Stop::unsupported_system;
        auto next=config;
        std::span<const std::uint8_t> payload;
        NativePacketBatch batch;
        if (mode==4) {
            if (config[2]) return Stop::unsupported_system;
            const auto result=batch.chain(memory,config[3],(value&0x40)!=0,(value&0x80)!=0);
            if (result!=Stop::budget) return result;
            payload=batch.bytes;
            next[1]=batch.source; next[3]=batch.tag;
            next[0]=(value&0xffffU)|batch.tag_bits;
        } else {
            const auto size=config[2]*16U;
            const auto result=NativePacketBatch::view(memory,config[1],size,payload);
            if (result!=Stop::budget) return result;
            next[1]=config[1]+size; next[0]=value;
        }
        const auto result=consumer.consume(consumer.context,channel,payload);
        if (result!=Stop::budget) return result;
        next[0]&=~0x100U; next[2]=0; config=next;
        memory.completions->complete(channel); memory.completions->publish(memory);
        return Stop::budget;
    }
private:
    static bool decode(std::uint32_t address,unsigned& channel,unsigned& field) {
        if (!maps(address) || address%16) return false;
        channel=(address-0x10008000)/0x1000; field=(address&0xfff)/16;
        return field<=5 || field==8;
    }
    std::array<std::array<std::uint32_t,9>,3> channels_{};
};
}
