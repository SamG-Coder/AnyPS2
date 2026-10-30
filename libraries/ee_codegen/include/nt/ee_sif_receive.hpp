#pragma once
#include <nt/ee_completions.hpp>
#include <nt/ee_handlers.hpp>
#include <span>
#include <cstring>

namespace nt::ee {
// Direct native packet destination. The compatibility view configures reception;
// no DMA tags are fetched, no FIFO is clocked, and no packet is synthesized.
class NativeSifReceive {
public:
    static bool maps(std::uint32_t address) {
        return address==0x1000c000 || address==0x1000c010 || address==0x1000c020;
    }
    void stop() { control_&=~0x100U; }
    bool accepting() const { return (control_&0x100)!=0; }
    IoRead read(std::uint32_t address) const {
        if (address==0x1000c000) return {Stop::budget,control_};
        if (address==0x1000c010) return {Stop::budget,destination_};
        if (address==0x1000c020) return {Stop::budget,count_};
        return {Stop::memory_fault};
    }
    Stop write(std::uint32_t address,std::uint32_t value) {
        if (address==0x1000c000) {
            // Receive direction, normal or chain mode, optional tag interrupt.
            if (value&0xfffffe7bU) return Stop::unsupported_system;
            if ((value&0x104)==0x104 && count_) return Stop::unsupported_system;
            if (accepting() && (value&0x100)) return Stop::unsupported_system;
            control_=value; return Stop::budget;
        }
        if (!maps(address)) return Stop::memory_fault;
        if (accepting()) return Stop::unsupported_system;
        if (address==0x1000c010) destination_=value;
        else count_=value&0xffffU;
        return Stop::budget;
    }
    Stop validate(Memory& memory,CodeOverlap protection,std::uint32_t packet_destination,std::size_t size) const {
        if (!accepting()) return Stop::unsupported_system;
        const bool chain=(control_&4)!=0;
        if ((!chain && size!=std::uint64_t(count_)*16) || size>0xffff0 || size%16)
            return Stop::invalid_state;
        const auto raw=chain ? packet_destination : destination_;
        const auto address=runtime::default_ram_address(raw);
        if (address%16 || std::uint64_t(address)+size>0x2000000U ||
            std::uint64_t(address)+size>memory.size || (size!=0 && !memory.data)) {
            memory.fault_address=raw; return Stop::memory_fault;
        }
        if (!protection) return Stop::invalid_state;
        if (size!=0 && protection(address,static_cast<std::uint32_t>(size))) {
            memory.fault_address=raw; return Stop::code_write;
        }
        return Stop::budget;
    }
    Stop deliver(Memory& memory,CodeOverlap protection,NativeCompletions& completions,
                 std::uint32_t packet_destination,std::span<const std::uint8_t> bytes,
                 bool terminal=true,bool tag_interrupt=false) {
        const auto valid=validate(memory,protection,packet_destination,bytes.size());
        if (valid!=Stop::budget) return valid;
        const bool chain=(control_&4)!=0;
        const auto address=runtime::default_ram_address(chain ? packet_destination : destination_);
        // memmove also permits a native producer whose owned data aliases RAM.
        if (!bytes.empty()) std::memmove(memory.data+address,bytes.data(),bytes.size());
        destination_=address+static_cast<std::uint32_t>(bytes.size()); count_=0;
        if (chain) control_=(control_&0xffffU)|(terminal ? 0x70000000U : 0x10000000U)|(tag_interrupt ? 0x80000000U : 0U);
        if (!chain || terminal || (tag_interrupt && (control_&0x80))) {
            control_&=~0x100U; completions.complete(5);
        }
        completions.publish(memory);
        return Stop::budget;
    }
private:
    std::uint32_t control_=0,destination_=0,count_=0;
};
}
