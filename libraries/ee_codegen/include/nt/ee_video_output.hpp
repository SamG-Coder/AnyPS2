#pragma once
#include <nt/ee_packet_batch.hpp>
#include <nt/ee_handlers.hpp>
#include <nt/ee_completions.hpp>

namespace nt::ee {
// A bounded destination reservation for native decoded media. Completion means
// bytes were copied, not merely that a destination was registered.
class NativeVideoOutput {
public:
    CodeOverlap protection=nullptr;
    static bool maps(std::uint32_t address) { return address>=0x1000b000 && address<=0x1000b080; }
    IoRead read(std::uint32_t address) const {
        if (address%16) return {Stop::unsupported_system};
        switch (address&0xff) {
        case 0: return {Stop::budget,control_};
        case 0x10: return {Stop::budget,destination_};
        case 0x20: return {Stop::budget,remaining_};
        default: return {Stop::budget,0};
        }
    }
    Stop write(Memory& memory,std::uint32_t address,std::uint32_t value) {
        if (address%16) return Stop::unsupported_system;
        const auto field=address&0xff;
        if (field==0 && !(value&0x100)) { control_=value; return Stop::budget; }
        if (control_&0x100) return Stop::unsupported_system;
        if (field==0x10) { destination_=value; return Stop::budget; }
        if (field==0x20) { remaining_=value&0xffffU; return Stop::budget; }
        if (field) return value ? Stop::unsupported_system : Stop::budget;
        // Only normal fixed-direction output is supported. Empty transfers have
        // console underflow behavior and are not successful native operations.
        if ((value&~0x101U) || !remaining_ || !memory.completions) return Stop::unsupported_system;
        std::span<const std::uint8_t> target;
        const auto status=NativePacketBatch::view(memory,destination_,remaining_*16,target);
        if (status!=Stop::budget) return status;
        if (!(destination_&0x80000000U)) {
            if (!protection) return Stop::invalid_state;
            if (protection(runtime::default_ram_address(destination_),remaining_*16)) {
                memory.fault_address=destination_; return Stop::code_write;
            }
        }
        control_=value;
        return Stop::budget;
    }
    std::size_t deliver(Memory& memory,bool enabled,std::span<const std::uint8_t> bytes) {
        if (!enabled || !(control_&0x100)) return 0;
        const auto size=std::min<std::size_t>(remaining_*16,bytes.size())&~std::size_t(15);
        if (!size) return 0;
        const bool scratch=(destination_&0x80000000U)!=0;
        auto* base=scratch ? memory.scratchpad : memory.data;
        const auto offset=scratch ? destination_&0x3fffU : runtime::default_ram_address(destination_);
        std::memcpy(base+offset,bytes.data(),size);
        destination_+=static_cast<std::uint32_t>(size); remaining_-=static_cast<std::uint32_t>(size/16);
        if (!remaining_) {
            control_&=~0x100U;
            memory.completions->complete(3); memory.completions->publish(memory);
        }
        return size;
    }
private:
    std::uint32_t control_=0,destination_=0,remaining_=0;
};
}
