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
        // Drained MFIFO bytes belong to the consumer, so the ring position is
        // already final while it works through them.
        if (channel==mfifo_channel && mfifo_running_) return {Stop::budget,channels_[channel][field]};
        if (pending_[channel]) {
            if (consumer.accepted) return {Stop::budget,position(channel)[field]};
            if (field>=1 && field<=5) return {Stop::unsupported_system};
        }
        return {Stop::budget,channels_[channel][field]};
    }
    // The ring's fill position advanced. Forward complete descriptors.
    Stop mfifo_filled(Memory& memory,std::uint32_t position) {
        mfifo_end_=position;
        return mfifo_drain(memory);
    }
    void mfifo_fill_position(std::uint32_t position) { mfifo_end_=position; }
    // Retry work a busy consumer refused earlier.
    Stop mfifo_retry(Memory& memory) { return mfifo_running_ ? mfifo_drain(memory) : Stop::budget; }
    Stop write(Memory& memory,std::uint32_t address,std::uint32_t value) {
        unsigned channel,field;
        if (!decode(address,channel,field)) return Stop::unsupported_system;
        if (channel==mfifo_channel && !field && (value&0x100) && policy.mfifo_to_vector() && direction_==Direction::selectable) {
            // The channel stays active and drains whatever the ring holds. Its
            // chain carries descriptors with transferred tags and no interrupts.
            if ((value&7U)!=5U || (value&0x80) || pending_[channel] || !consumer.consume_with_origins || !memory.completions)
                return Stop::unsupported_system;
            channels_[channel][0]=value;
            mfifo_running_=true;
            return mfifo_drain(memory);
        }
        if (channel==mfifo_channel && !field && !(value&0x100)) mfifo_running_=false;
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
        if (channel==mfifo_channel && mfifo_running_) {
            pending_[channel].reset();
            mfifo_status_=mfifo_drain(memory);
            return;
        }
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
    // A failure found while draining outside a guest access reports at the next one.
    Stop take_mfifo_status() { const auto status=mfifo_status_; mfifo_status_=Stop::budget; return status; }
private:
    static constexpr unsigned mfifo_channel=1;
    Stop mfifo_drain(Memory& memory) {
        if (!mfifo_running_ || pending_[mfifo_channel]) return Stop::budget;
        auto& config=channels_[mfifo_channel];
        const auto used=[&](std::uint32_t from) {
            return (policy.mfifo_address(mfifo_end_)-from)&(policy.mfifo_address(0xffffffffU)-policy.mfifo_address(0U)+15U);
        };
        const auto ring=[&](std::uint32_t address) { return policy.mfifo_address(address); };
        NativePacketBatch batch;
        const auto append=[&](std::uint32_t address,std::uint32_t size,bool in_ring)->Stop {
            while (size) {
                const auto chunk=in_ring ? std::min(size,policy.mfifo_address(0xffffffffU)+16U-address) : size;
                std::span<const std::uint8_t> bytes;
                const auto viewed=NativePacketBatch::view(memory,address,chunk,bytes);
                if (viewed!=Stop::budget) return viewed;
                batch.bytes.insert(batch.bytes.end(),bytes.begin(),bytes.end());
                size-=chunk;
                address=in_ring ? ring(address+chunk) : address+chunk;
            }
            return Stop::budget;
        };
        auto tag=ring(config[3]);
        bool ended=false;
        std::uint32_t bits=config[0]&0xffff0000U;
        // Bounded so a very long ring still reaches the consumer in order.
        while (tag!=ring(mfifo_end_) && batch.bytes.size()<0x100000U) {
            std::span<const std::uint8_t> descriptor;
            const auto viewed=NativePacketBatch::view(memory,tag,16,descriptor);
            if (viewed!=Stop::budget) return viewed;
            std::uint32_t words[2]; std::memcpy(words,descriptor.data(),sizeof(words));
            const auto kind=(words[0]>>28)&7U,size=(words[0]&0xffffU)*16U;
            const auto after=ring(tag+16);
            const bool inline_payload=kind==1 || kind==2 || kind==7;
            if (kind==5 || kind==6) return Stop::unsupported_system;
            // Wait for the rest of a descriptor; its filler is still in progress.
            if (inline_payload && used(after)<size) break;
            batch.origins.push_back({batch.bytes.size(),8});
            batch.bytes.insert(batch.bytes.end(),descriptor.end()-8,descriptor.end());
            if (size) batch.origins.push_back({batch.bytes.size(),0});
            const auto copied=inline_payload ? append(after,size,true) : append(words[1],size,false);
            if (copied!=Stop::budget) return copied;
            bits=words[0]&0xffff0000U;
            config[1]=inline_payload ? ring(after+size) : words[1]+size;
            tag=kind==1 ? ring(after+size) : kind==2 ? ring(words[1]) : after;
            if (kind==0 || kind==7) { ended=true; break; }
        }
        if (batch.bytes.empty()) return Stop::budget;
        const auto result=consumer.consume_with_origins(consumer.context,mfifo_channel,batch.bytes,batch.origins);
        // A busy consumer keeps its state unchanged. The fill loop polls the channel
        // position before it overwrites anything, which retries this batch.
        if (result==Stop::native_wait || result==Stop::vector_pending) return Stop::budget;
        if (result!=Stop::budget) return result;
        config[3]=tag;
        config[0]=(config[0]&0xffffU)|bits;
        if (consumer.pending && consumer.pending(consumer.context,mfifo_channel)) pending_[mfifo_channel]=config;
        if (ended && !pending_[mfifo_channel]) {
            mfifo_running_=false;
            config[0]&=~0x100U; config[2]=0;
            memory.completions->complete(completion_base_+mfifo_channel); memory.completions->publish(memory);
        }
        return Stop::budget;
    }
    bool mfifo_running_=false;
    std::uint32_t mfifo_end_=0;
    Stop mfifo_status_=Stop::budget;
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
