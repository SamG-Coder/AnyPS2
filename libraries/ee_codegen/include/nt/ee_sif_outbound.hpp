#pragma once
#include <nt/ee_completions.hpp>
#include <span>
#include <vector>
#include <cstring>

namespace nt::ee {
struct NativeSifPacket {
    std::uint32_t destination=0,attributes=0;
    std::vector<std::uint8_t> bytes;
};
// Peer acceptance must be atomic across the entire immutable batch. budget/0
// means backpressure, budget/1 means consumed; failures must preserve peer state.
struct NativeSifSink {
    void* context=nullptr;
    IoRead (*accept)(void*,std::span<const NativeSifPacket>)=nullptr;
};
class NativeSifOutbound {
public:
    bool connect(NativeSifSink sink) {
        if (active_ || !sink.accept) return false;
        sink_=sink; return true;
    }
    IoRead submit(Memory& memory,std::uint32_t raw,std::uint32_t count) {
        if (!sink_.accept) return {Stop::unsupported_system};
        if (active_) return {Stop::budget,0};
        if (!count || count>32 || raw%4) return {Stop::invalid_state};
        const auto address=physical(raw);
        if (!range(memory,address,std::uint64_t(count)*16)) { memory.fault_address=raw; return {Stop::memory_fault}; }
        if (last_id_==0x7fffffffU) return {Stop::unsupported_system};
        std::vector<NativeSifPacket> batch;
        std::size_t total=0;
        for (std::uint32_t index=0;index<count;++index) {
            std::uint32_t fields[4]; std::memcpy(fields,memory.data+address+index*16,16);
            const auto source=physical(fields[0]),destination=fields[1],size=fields[2],attributes=fields[3];
            if (source%16 || destination%4 || size>0xffff0 || (attributes&~0x44U)) return {Stop::invalid_state};
            const auto padded=(size+15U)&~15U;
            if (!range(memory,source,padded)) { memory.fault_address=fields[0]; return {Stop::memory_fault}; }
            if (std::uint64_t(destination)+padded>0x200000U) { memory.fault_address=destination; return {Stop::memory_fault}; }
            if (padded>0x100000-total) return {Stop::invalid_state};
            total+=padded;
            batch.push_back({destination,attributes,{memory.data+source,memory.data+source+padded}});
        }
        packets_=std::move(batch); active_=true;
        return {Stop::budget,++last_id_};
    }
    std::uint32_t status(std::uint32_t id) const { return id && id==last_id_ && active_ ? 1U : 0xffffffffU; }
    Stop pump(Memory& memory) {
        if (!active_) return Stop::budget;
        if (!memory.completions) return Stop::unsupported_system;
        const auto result=sink_.accept(sink_.context,packets_);
        if (result.status!=Stop::budget) return result.status;
        if (!result.value) return Stop::budget;
        if (result.value!=1) return Stop::invalid_state;
        packets_.clear(); active_=false;
        memory.completions->complete(6); memory.completions->publish(memory);
        return Stop::budget;
    }
private:
    static std::uint32_t physical(std::uint32_t value) { return runtime::default_ram_address(value); }
    static bool range(const Memory& memory,std::uint32_t address,std::uint64_t size) {
        return memory.data && std::uint64_t(address)+size<=memory.size && std::uint64_t(address)+size<=0x2000000U;
    }
    NativeSifSink sink_;
    std::vector<NativeSifPacket> packets_;
    std::uint32_t last_id_=0;
    bool active_=false;
};
}
