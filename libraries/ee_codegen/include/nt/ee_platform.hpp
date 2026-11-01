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
namespace nt::ee {
class NativePlatform {
public:
    explicit NativePlatform(NativeTimers::Clock clock=nullptr) : graphics(clock),timers(clock) {
        timers.connect_events(); packet_submission.consumer={this,consume_packet,packet_pending};
    }
    NativePlatform(const NativePlatform&)=delete;
    NativePlatform& operator=(const NativePlatform&)=delete;
    static runtime::State initial_state(std::uint32_t entry) {
        runtime::State state; state.pc=entry;
        // Native application context: INTC/DMA classes and global delivery are
        // available. EIE stays clear until compiled startup explicitly enables it.
        state.cop0_status=0xc01;
        return state;
    }
    // Optional synchronous host debug-byte sink. With no pending host writes,
    // the transmit-full flag is clear; no console serial timing is simulated.
    void* debug_context=nullptr;
    Stop (*debug_byte)(void*,std::uint8_t)=nullptr;
    NativeVectorOwner vector_owner;
    NativeGraphics graphics;
    NativePacketSetup packet_setup;
    NativePacketSubmission packet_submission;
    NativeInterrupts interrupts;
    NativeTimers timers;
    NativeCompletions completions;
    NativeSifReceive sif_receive;
    NativeSifLink sif_link{sif_receive};
    NativeSifOutbound sif_outbound;
    void attach(Memory& memory) { memory_=&memory; memory.vector_control=vector_owner.interface(); memory.vectors=&vector_owner.storage; memory.graphics=&graphics; memory.scratchpad=scratchpad_.data(); memory.scratchpad_bytes=scratchpad_.size(); memory.interrupts=&interrupts; memory.completions=&completions; memory.sif_link=&sif_link; memory.sif_outbound=&sif_outbound; memory.io={this,read,write,poll,read64,write64,write8,test_completions}; }
private:
    static IoRead test_completions(void* context) {
        const auto& self=*static_cast<NativePlatform*>(context);
        return {Stop::budget,self.completions.contains(self.packet_submission.policy.completion_selection()) ? 1U : 0U};
    }
    static Stop consume_packet(void* context,unsigned channel,std::span<const std::uint8_t> bytes) {
        auto& self=*static_cast<NativePlatform*>(context);
        return self.packet_setup.consume(channel,bytes,self.vector_owner.storage.active);
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
        }
    }
    static IoRead64 read64(void* context,std::uint32_t address) {
        if (address<0x12000000 || address>=0x12002000) return {Stop::memory_fault};
        return static_cast<NativePlatform*>(context)->graphics.read(address);
    }
    static Stop write64(void* context,std::uint32_t address,std::uint64_t value) {
        if (address<0x12000000 || address>=0x12002000) return Stop::memory_fault;
        return static_cast<NativePlatform*>(context)->graphics.write(address,value);
    }
    static bool event_address(std::uint32_t address) { return address==0x1000f000 || address==0x1000f010; }
    static IoRead read(void* context,std::uint32_t address) {
        auto& self=*static_cast<NativePlatform*>(context);
        if (address==0x1000f130)
            return {self.debug_byte ? Stop::budget : Stop::unsupported_system,0};
        if (NativeGraphics::maps(address)) {
            const auto value=self.graphics.read(address); return {value.status,static_cast<std::uint32_t>(value.value)};
        }
        if (NativePacketSetup::maps(address)) return self.packet_setup.read(address);
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
        if (NativeGraphics::maps(address)) return self.graphics.write(address,value);
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
        if (NativeSubmissionPolicy::maps(address)) return self.packet_submission.policy.write(address,value);
        if (NativePacketSubmission::maps(address)) return self.memory_ ? self.packet_submission.write(*self.memory_,address,value) : Stop::invalid_state;
        if (NativeSifReceive::maps(address)) return self.sif_receive.write(address,value);
        if (address==0x1000e010) { self.completions.acknowledge(value); return Stop::budget; }
        if (event_address(address)) return self.interrupts.write(address,value);
        const auto io=self.timers.interface(); return io.write32(io.context,address,value);
    }
};
}
