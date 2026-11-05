#pragma once
#include <nt/ee_completions.hpp>
#include <nt/ee_submission_policy.hpp>
#include <nt/ee_packet_batch.hpp>
#include <nt/ee_handlers.hpp>
#include <array>
#include <optional>
#include <span>
#include <stdexcept>

namespace nt::ee {
// A native packet consumer. Success means all bytes were consumed or retained;
// every other result must leave consumer state unchanged. The view is borrowed
// only for this call. A pending consumer owns its bytes until complete() or
// cancel(). Descriptor batches have no transfer clock.
struct NativePacketConsumer {
    void* context=nullptr;
    Stop (*consume)(void*,unsigned,std::span<const std::uint8_t>)=nullptr;
    bool (*pending)(void*,unsigned)=nullptr;
    Stop (*consume_with_origins)(void*,unsigned,std::span<const std::uint8_t>,std::span<const NativePacketOrigin>)=nullptr;
    bool (*working)(void*)=nullptr;
    std::size_t (*accepted)(void*,unsigned)=nullptr;
    void (*cancel)(void*,unsigned)=nullptr;
    Stop (*receive)(void*,unsigned,std::span<std::uint8_t>)=nullptr;
};
class NativePacketSubmission {
public:
    enum class Direction { selectable, fixed_input };
    explicit NativePacketSubmission(unsigned completion_base=0,Direction direction=Direction::selectable)
        : completion_base_(completion_base),direction_(direction) {
        if (completion_base>7) throw std::invalid_argument("Native submission completion range exceeds available events");
    }
    NativePacketConsumer consumer;
    CodeOverlap protection=nullptr;
    NativeSubmissionPolicy policy;
    static bool maps(std::uint32_t address) { return address>=0x10008000 && address<0x1000b000; }
    IoRead read(std::uint32_t address) const {
        unsigned channel,field;
        if (!decode(address,channel,field)) return {Stop::unsupported_system};
        if (pending_[channel]) {
            if (consumer.accepted) return {Stop::budget,position(channel)[field]};
            if (field>=1 && field<=5) return {Stop::unsupported_system};
        }
        return {Stop::budget,channels_[channel][field]};
    }
    Stop write(Memory& memory,std::uint32_t address,std::uint32_t value) {
        unsigned channel,field;
        if (!decode(address,channel,field)) return Stop::unsupported_system;
        if (pending_[channel]) {
            if (!field && !(value&0x100) && direction_==Direction::fixed_input && consumer.accepted && consumer.cancel) {
                auto next=position(channel);
                consumer.cancel(consumer.context,channel);
                next[0]=(next[0]&0xffff0000U)|(value&0xffffU);
                channels_[channel]=next; pending_[channel].reset(); chunks_[channel].clear();
                return Stop::budget;
            }
            return consumer.working && consumer.working(consumer.context) ? Stop::native_wait : Stop::unsupported_system;
        }
        auto& config=channels_[channel];
        if (field==8) return value ? Stop::unsupported_system : Stop::budget;
        if (field) {
            config[field]=field==2 ? value&0xffffU : value;
            return Stop::budget;
        }
        // Idle configuration is metadata. Starting work requires a native
        // consumer, normal forward transfer and a complete bounded source.
        if (!(value&0x100)) { config[0]=value; armed_[channel]=false; return Stop::budget; }
        const auto mode=value&0xc;
        // Only the second channel transfers in both directions. The others have
        // one fixed direction, and hardware ignores their DIR bit.
        if (direction_==Direction::selectable && channel==1 && !(value&1)) {
            // TAG/ASP/TTE/TIE describe chain traversal; they are retained
            // metadata in normal mode and do not alter this byte transfer.
            if ((value&0x0000fe0fU) || !config[2] || !policy.enabled() ||
                !consumer.receive || !memory.completions) return Stop::unsupported_system;
            const auto size=config[2]*16U;
            std::span<const std::uint8_t> target;
            const auto checked=NativePacketBatch::view(memory,config[1],size,target);
            if (checked!=Stop::budget) return checked;
            const bool scratch=(config[1]&0x80000000U)!=0;
            if (!scratch) {
                if (!protection) return Stop::invalid_state;
                if (protection(runtime::default_ram_address(config[1]),size)) {
                    memory.fault_address=config[1]; return Stop::code_write;
                }
            }
            std::vector<std::uint8_t> output(size);
            const auto received=consumer.receive(consumer.context,channel,output);
            if (received!=Stop::budget) return received;
            auto* base=scratch ? memory.scratchpad : memory.data;
            const auto offset=scratch ? config[1]&0x3fffU : runtime::default_ram_address(config[1]);
            std::memcpy(base+offset,output.data(),size);
            config[0]=value&~0x100U; config[1]+=size; config[2]=0;
            memory.completions->complete(completion_base_+channel); memory.completions->publish(memory);
            return Stop::budget;
        }
        if ((mode!=0 && mode!=4) ||
            (value&0x0000fe02U) || ((value>>4)&3)==3 || ((channel==2 || direction_==Direction::fixed_input) && (value&0x40)) || !consumer.consume || !memory.completions)
            return Stop::unsupported_system;
        if (!policy.enabled()) {
            if (direction_!=Direction::fixed_input) return Stop::unsupported_system;
            // Retain the request, but do not read its source or complete it
            // until the native queue owner resumes submissions.
            config[0]=value; armed_[channel]=true; return Stop::budget;
        }
        auto next=config;
        std::span<const std::uint8_t> payload;
        NativePacketBatch batch;
        if (mode==4) {
            batch.stack={{config[4],config[5]},(value>>4)&3U};
            bool terminal=false;
            if (config[2]) {
                if (direction_!=Direction::fixed_input) return Stop::unsupported_system;
                // Resume the unread payload of an already accepted descriptor.
                // Its tag bytes were submitted earlier; do not submit them twice.
                const auto result=NativePacketBatch::view(memory,config[1],config[2]*16U,payload);
                if (result!=Stop::budget) return result;
                if (payload.size()>NativePacketBatch::maximum_bytes) return Stop::invalid_state;
                batch.bytes.assign(payload.begin(),payload.end());
                batch.origins.push_back({0,0});
                batch.chunks.push_back({0,payload.size(),config[1],config[3],value&0xffff0000U,batch.stack});
                batch.source=config[1]+config[2]*16U; batch.tag=config[3]; batch.tag_bits=value&0xffff0000U;
                const auto kind=(value>>28)&7;
                terminal=kind==0 || kind==7 || ((value&0x80) && (value&0x80000000U));
            }
            if (!terminal) {
                const auto result=batch.chain(memory,config[3],(value&0x40)!=0,(value&0x80)!=0,8,NativePacketBatch::view,true,direction_==Direction::fixed_input);
                if (result!=Stop::budget) return result;
            }
            payload=batch.bytes;
            next[1]=batch.source; next[3]=batch.tag;
            next[0]=(value&0xffcfU)|batch.tag_bits|(batch.stack.size<<4);
            next[4]=batch.stack.returns[0]; next[5]=batch.stack.returns[1];
        } else {
            const auto size=config[2]*16U;
            const auto result=NativePacketBatch::view(memory,config[1],size,payload);
            if (result!=Stop::budget) return result;
            next[1]=config[1]+size; next[0]=value;
            if (direction_==Direction::fixed_input) batch.chunks.push_back({0,size,config[1],config[3],value&0xffff0000U,batch.stack});
        }
        const auto result=consumer.consume_with_origins ? consumer.consume_with_origins(consumer.context,channel,payload,batch.origins) :
            consumer.consume(consumer.context,channel,payload);
        if (result!=Stop::budget) return result;
        armed_[channel]=false;
        if (consumer.pending && consumer.pending(consumer.context,channel)) {
            chunks_[channel]=std::move(batch.chunks); pending_[channel]=next; config[0]=value; return Stop::budget;
        }
        next[0]&=~0x100U; next[2]=0; config=next;
        memory.completions->complete(completion_base_+channel); memory.completions->publish(memory);
        return Stop::budget;
    }
    Stop resume(Memory& memory) {
        if (!policy.enabled()) return Stop::budget;
        for (unsigned channel=0;channel<armed_.size();++channel) if (armed_[channel]) {
            const auto status=write(memory,0x10008000+channel*0x1000,channels_[channel][0]);
            if (status!=Stop::budget) return status;
        }
        return Stop::budget;
    }
    void complete(Memory& memory,unsigned channel) {
        if (channel>=pending_.size() || !pending_[channel]) return;
        auto next=*pending_[channel]; next[0]&=~0x100U; next[2]=0;
        channels_[channel]=next; pending_[channel].reset(); chunks_[channel].clear();
        if (memory.completions) {
            memory.completions->complete(completion_base_+channel); memory.completions->publish(memory);
        }
    }
    void cancel(unsigned channel) {
        if (channel>=pending_.size() || !pending_[channel]) return;
        pending_[channel].reset(); channels_[channel][0]&=~0x100U; channels_[channel][2]=0;
    }
private:
    std::array<std::uint32_t,9> position(unsigned channel) const {
        auto current=channels_[channel];
        const auto accepted=consumer.accepted(consumer.context,channel);
        for (const auto& chunk:chunks_[channel]) if (accepted<chunk.end) {
            const auto offset=accepted-chunk.begin;
            current[0]=(current[0]&0xffcfU)|chunk.tag_bits|(chunk.stack.size<<4);
            current[1]=chunk.source+static_cast<std::uint32_t>(offset);
            current[2]=static_cast<std::uint32_t>((chunk.end-accepted)/16);
            current[3]=chunk.next_tag; current[4]=chunk.stack.returns[0]; current[5]=chunk.stack.returns[1];
            return current;
        }
        current=*pending_[channel]; current[2]=0;
        return current;
    }
    unsigned completion_base_=0;
    Direction direction_=Direction::selectable;
    static bool decode(std::uint32_t address,unsigned& channel,unsigned& field) {
        if (!maps(address) || address%16) return false;
        channel=(address-0x10008000)/0x1000; field=(address&0xfff)/16;
        return field<=5 || field==8;
    }
    std::array<std::array<std::uint32_t,9>,3> channels_{};
    std::array<std::optional<std::array<std::uint32_t,9>>,3> pending_{};
    std::array<bool,3> armed_{};
    std::array<std::vector<NativePacketBatch::Chunk>,3> chunks_;
};
}
