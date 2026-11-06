#pragma once
#include <nt/ee_interrupts.hpp>
#include <nt/ee_timers.hpp>
#include <nt/ee_completions.hpp>
#include <nt/ee_sif_link.hpp>
#include <nt/ee_sif_outbound.hpp>
#include <nt/ee_graphics.hpp>
#include <nt/ee_packet_setup.hpp>
#include <nt/ee_packet_submission.hpp>
#include <nt/ee_vector_owner.hpp>
#include <nt/ee_scratchpad_copy.hpp>
#include <nt/ee_video.hpp>
#include <nt/ee_video_output.hpp>
namespace nt::ee {
class NativePlatform {
public:
    explicit NativePlatform(NativeTimers::Clock clock=nullptr) : graphics(clock),timers(clock) {
        timers.connect_events(); packet_submission.consumer={this,consume_packet,packet_pending,consume_packet_origins};
        timers.display_context=&graphics;
        timers.display_lines=[](void* raw,std::uint64_t time) {
            return static_cast<NativeGraphics*>(raw)->horizontal_ticks(time);
        };
        packet_submission.consumer.receive=[](void* raw,unsigned channel,std::span<std::uint8_t> output) {
            auto& self=*static_cast<NativePlatform*>(raw);
            if (channel!=1 || !self.packet_setup.reverse_output() || !self.graphics.readback_enabled() || !self.graphics.readback)
                return Stop::unsupported_system;
            return self.graphics.readback(self.graphics.context,output);
        };
        packet_submission.consumer.working=[](void* raw) { return static_cast<NativePlatform*>(raw)->packet_setup.working(); };
        // Application startup owns a usable native submission queue. Explicit
        // guest suspension still gates it; completion requires real consumption.
        packet_submission.policy.enable_native_queue();
        video_submission.consumer={this,[](void* raw,unsigned,std::span<const std::uint8_t> bytes) {
            return static_cast<NativePlatform*>(raw)->video.submit(bytes);
        },[](void* raw,unsigned) { return static_cast<NativePlatform*>(raw)->video.submission_pending(); }};
        video_submission.consumer.accepted=[](void* raw,unsigned) { return static_cast<NativePlatform*>(raw)->video.accepted_bytes(); };
        video_submission.consumer.cancel=[](void* raw,unsigned) { static_cast<NativePlatform*>(raw)->video.cancel_input(); };
    }
    NativePlatform(const NativePlatform&)=delete;
    NativePlatform& operator=(const NativePlatform&)=delete;
    Stop poll_submissions() {
        for (unsigned channel=0;channel<2;++channel) {
            const bool pending=packet_setup.pending(channel);
            const auto result=packet_setup.poll(channel,vector_owner.storage.active);
            if (result!=Stop::budget) return result;
            if (pending && !packet_setup.pending(channel) && memory_) packet_submission.complete(*memory_,channel);
        }
        return Stop::budget;
    }
    static runtime::State initial_state(std::uint32_t entry) {
        runtime::State state; state.pc=entry;
        // Native application context: INTC/DMA classes and global delivery are
        // available. EDI permits application DI/EI after SDK startup enters user
        // mode. EIE stays clear until compiled startup explicitly enables it.
        state.cop0_status=0x20c01;
        return state;
    }
    // Optional synchronous host debug-byte sink. With no pending host writes,
    // the transmit-full flag is clear; no console serial timing is simulated.
    void* debug_context=nullptr;
    Stop (*debug_byte)(void*,std::uint8_t)=nullptr;
    NativeVectorOwner vector_owner;
    NativeGraphics graphics;
    NativeVideo video;
    NativeVideoOutput video_output;
    NativePacketSetup packet_setup;
    NativePacketSubmission packet_submission;
    NativePacketSubmission video_submission{4,NativePacketSubmission::Direction::fixed_input};
    NativeScratchpadCopy scratchpad_copy;
    NativeInterrupts interrupts;
    NativeTimers timers;
    NativeCompletions completions;
    NativeSifReceive sif_receive;
    NativeSifLink sif_link{sif_receive};
    NativeSifOutbound sif_outbound;
    void attach(Memory& memory) { memory_=&memory; memory.vector_control=vector_owner.interface(); memory.vectors=&vector_owner.storage; memory.graphics=&graphics; memory.scratchpad=scratchpad_.data(); memory.scratchpad_bytes=scratchpad_.size(); memory.interrupts=&interrupts; memory.completions=&completions; memory.sif_link=&sif_link; memory.sif_outbound=&sif_outbound; memory.io={this,read,write,poll,read64,write64,write8,test_completions,write128}; }
private:
    static IoRead test_completions(void* context) {
        const auto& self=*static_cast<NativePlatform*>(context);
        return {Stop::budget,self.completions.contains(self.packet_submission.policy.completion_selection()) ? 1U : 0U};
    }
    static Stop consume_packet(void* context,unsigned channel,std::span<const std::uint8_t> bytes) {
        return consume_packet_origins(context,channel,bytes,{});
    }
    static Stop consume_packet_origins(void* context,unsigned channel,std::span<const std::uint8_t> bytes,std::span<const NativePacketOrigin> origins) {
        auto& self=*static_cast<NativePlatform*>(context);
        return self.packet_setup.consume(channel,bytes,self.vector_owner.storage.active,origins);
    }
    static bool packet_pending(void* context,unsigned channel) {
        return static_cast<NativePlatform*>(context)->packet_setup.pending(channel);
    }
    Memory* memory_=nullptr;
    std::vector<std::uint8_t> scratchpad_=std::vector<std::uint8_t>(scratchpad_size,0);
    static void poll(void* context) {
        auto& self=*static_cast<NativePlatform*>(context);
        auto events=self.timers.poll()|self.graphics.poll()|(self.packet_setup.take_interrupts()<<4);
        while (events) {
            const auto line=static_cast<unsigned>(std::countr_zero(events));
            self.interrupts.raise(line); events&=events-1;
            if (line==2 && self.memory_) if (const auto target=self.interrupts.take_display_flag()) {
                const std::uint32_t flag=1;
                const auto status=self.graphics.read(0x12001000);
                std::memcpy(self.memory_->data+target->flag,&flag,sizeof(flag));
                std::memcpy(self.memory_->data+target->status,&status.value,sizeof(status.value));
            }
        }
    }
    static IoRead64 read64(void* context,std::uint32_t address) {
        auto& self=*static_cast<NativePlatform*>(context);
        if (NativeVideo::maps(address)) {
            const auto low=self.video.read(address),high=self.video.read(address+4);
            // Only CMD and TOP expose a 64-bit result/busy pair.
            if (address!=0x10002000 && address!=0x10002030) return {Stop::unsupported_system};
            return {low.status!=Stop::budget ? low.status : high.status,std::uint64_t(low.value)|(std::uint64_t(high.value)<<32)};
        }
        if (address<0x12000000 || address>=0x12002000) return {Stop::memory_fault};
        return self.graphics.read(address);
    }
    static Stop write128(void* context,std::uint32_t address,runtime::Register value) {
        if (address==0x10007010) {
            auto& self=*static_cast<NativePlatform*>(context);
            const auto bytes=std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(&value),sizeof(value));
            const auto result=self.video.append(bytes);
            if (result==Stop::budget) self.publish_video();
            return result;
        }
        // Programmed packet writes share the same native command consumer as
        // descriptor submissions, without inventing DMA completion events.
        if (address<0x10004000 || address>=0x10007000 || address%16) return Stop::memory_fault;
        auto& self=*static_cast<NativePlatform*>(context);
        const auto channel=(address-0x10004000)/0x1000;
        const auto bytes=std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(&value),sizeof(value));
        return self.packet_setup.consume(channel,bytes,self.vector_owner.storage.active);
    }
    static Stop write64(void* context,std::uint32_t address,std::uint64_t value) {
        if (NativeVideo::maps(address)) return write(context,address,static_cast<std::uint32_t>(value));
        if (address<0x12000000 || address>=0x12002000) return Stop::memory_fault;
        auto& self=*static_cast<NativePlatform*>(context);
        return self.write_graphics(address,value);
    }
    Stop write_graphics(std::uint32_t address,std::uint64_t value) {
        // Interrupt controls do not consume vector output, so they may change while a
        // compiled VU job is still producing geometry. Flush/reset affect the drawing
        // owner, and a display buffer change shows what was drawn before it: both must
        // await the publication of queued work.
        const bool buffer_change=(address==0x12000070 || address==0x12000090) && graphics.displayed_buffer((address-0x12000070)/32)!=value;
        if (((address==0x12001000 && (value&0x300)) || buffer_change) && packet_setup.working())
            return Stop::native_wait;
        return graphics.write(address,value);
    }
    static bool video_input_address(std::uint32_t address) { return address>=0x1000b400 && address<=0x1000b480; }
    static std::uint32_t video_input_descriptor(std::uint32_t address) { return 0x10008000+(address-0x1000b400); }
    void publish_video() {
        if (memory_) {
            while (const auto copied=video_output.deliver(*memory_,packet_submission.policy.enabled(),video.output()))
                video.consume_output(copied);
        }
        if (video.take_completion()) interrupts.raise(8);
        if (memory_ && !video.submission_pending()) video_submission.complete(*memory_,0);
    }
    static bool event_address(std::uint32_t address) { return address==0x1000f000 || address==0x1000f010; }
    static IoRead read(void* context,std::uint32_t address) {
        auto& self=*static_cast<NativePlatform*>(context);
        if (address==0x1000f130)
            return {self.debug_byte ? Stop::budget : Stop::unsupported_system,0};
        if (NativeVideoOutput::maps(address)) return self.video_output.read(address);
        if (video_input_address(address)) return self.video_submission.read(video_input_descriptor(address));
        if (NativeVideo::maps(address)) return self.video.read(address);
        if (NativeGraphics::maps(address)) {
            const auto value=self.graphics.read(address); return {value.status,static_cast<std::uint32_t>(value.value)};
        }
        if (address==0x10003c00 && self.packet_setup.reverse_output()) {
            if (!self.graphics.readback_status) return {Stop::unsupported_system};
            const auto available=self.graphics.readback_status(self.graphics.context);
            if (available.status!=Stop::budget) return available;
            auto status=self.packet_setup.read(address);
            status.value|=std::min(16U,available.value/16)<<24;
            return status;
        }
        if (NativePacketSetup::maps(address)) return self.packet_setup.read(address);
        if (NativeScratchpadCopy::maps(address)) return self.scratchpad_copy.read(address);
        if (NativeSubmissionPolicy::maps(address)) return self.packet_submission.policy.read(address);
        if (NativePacketSubmission::maps(address)) return self.packet_submission.read(address);
        if (NativeSifReceive::maps(address)) return self.sif_receive.read(address);
        if (address==0x1000e010) return {Stop::budget,self.completions.status()};
        if (event_address(address)) return self.interrupts.read(address);
        const auto io=self.timers.interface(); return io.read32(io.context,address);
    }
    static Stop write8(void* context,std::uint32_t address,std::uint8_t value) {
        auto& self=*static_cast<NativePlatform*>(context);
        if (address!=0x1000f180) return Stop::memory_fault;
        return self.debug_byte ? self.debug_byte(self.debug_context,value) : Stop::unsupported_system;
    }
    static Stop write(void* context,std::uint32_t address,std::uint32_t value) {
        auto& self=*static_cast<NativePlatform*>(context);
        if (address==0x1000f180)
            return self.debug_byte ? self.debug_byte(self.debug_context,static_cast<std::uint8_t>(value)) : Stop::unsupported_system;
        if (NativeVideoOutput::maps(address)) {
            if (!self.memory_) return Stop::invalid_state;
            const auto status=self.video_output.write(*self.memory_,address,value);
            if (status==Stop::budget) self.publish_video();
            return status;
        }
        if (video_input_address(address)) {
            if (!self.memory_) return Stop::invalid_state;
            self.video_submission.policy=self.packet_submission.policy;
            const auto result=self.video_submission.write(*self.memory_,video_input_descriptor(address),value);
            if (result==Stop::budget) self.publish_video();
            return result;
        }
        if (NativeVideo::maps(address)) {
            const auto result=self.video.write(address,value);
            if (result==Stop::budget) self.publish_video();
            return result;
        }
        if (NativeGraphics::maps(address)) return self.write_graphics(address,value);
        if (NativePacketSetup::maps(address)) {
            const auto result=self.packet_setup.write(address,value);
            if (result!=Stop::budget || (address!=0x10003810 && address!=0x10003c10)) return result;
            const auto channel=(address-0x10003810)/0x400;
            if (value&1) { self.packet_submission.cancel(channel); return Stop::budget; }
            if (!(value&8)) return Stop::budget;
            const auto resumed=self.packet_setup.resume(channel,self.vector_owner.storage.active);
            if (resumed==Stop::budget && !self.packet_setup.pending(channel) && self.memory_)
                self.packet_submission.complete(*self.memory_,channel);
            return resumed;
        }
        if (NativeScratchpadCopy::maps(address)) return self.memory_ ? self.scratchpad_copy.write(*self.memory_,self.packet_submission.policy,address,value) : Stop::invalid_state;
        if (NativeSubmissionPolicy::maps(address)) {
            const auto status=self.packet_submission.policy.write(address,value);
            if (status!=Stop::budget || !self.memory_) return status;
            self.video_submission.policy=self.packet_submission.policy;
            const auto resumed=self.video_submission.resume(*self.memory_);
            if (resumed==Stop::budget) self.publish_video();
            return resumed;
        }
        if (NativePacketSubmission::maps(address)) return self.memory_ ? self.packet_submission.write(*self.memory_,address,value) : Stop::invalid_state;
        if (NativeSifReceive::maps(address)) return self.sif_receive.write(address,value);
        if (address==0x1000e010) { self.completions.acknowledge(value); return Stop::budget; }
        if (event_address(address)) return self.interrupts.write(address,value);
        const auto io=self.timers.interface(); return io.write32(io.context,address,value);
    }
};
}
