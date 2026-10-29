#pragma once
#include <nt/ee_handlers.hpp>
#include <array>
#include <cstring>

namespace nt::ee {
// Application-owned preferences, not a firmware image or persistent console state.
struct ApplicationSettings {
    // Public ABI: English, version 1, 4:3, UTC, digital audio enabled.
    std::uint32_t primary=(1U<<16)|(1U<<13)|(1U<<4);
    std::array<std::uint8_t,4> extended{0,0,0,1};
};
class NativeSettings {
public:
    explicit NativeSettings(ApplicationSettings settings={}) : settings_(settings) {}
    Stop invoke(std::uint32_t call,const runtime::State& state,Memory& memory,CodeOverlap protection) {
        const bool extended=call==0x6e || call==0x6f;
        const bool output=call==0x4b || call==0x6f;
        if (!extended && call!=0x4a && call!=0x4b) return Stop::syscall;
        const auto raw=static_cast<std::uint32_t>(state.gpr[4].lo);
        const auto address=runtime::default_ram_address(raw);
        const auto size=extended ? static_cast<std::uint32_t>(state.gpr[5].lo) : 4U;
        const auto offset=extended ? static_cast<std::uint32_t>(state.gpr[6].lo) : 0U;
        if (offset>4 || size>4-offset) return Stop::invalid_state;
        if (!size) return Stop::budget;
        if (!memory.data || (!extended && address%4) ||
            std::uint64_t(address)+size>memory.size || std::uint64_t(address)+size>0x2000000U) {
            memory.fault_address=raw; return Stop::memory_fault;
        }
        if (output) {
            if (!protection) return Stop::invalid_state;
            if (protection(address,size)) { memory.fault_address=raw; return Stop::code_write; }
        }
        auto* storage=extended ? settings_.extended.data()+offset : reinterpret_cast<std::uint8_t*>(&settings_.primary);
        if (output) std::memcpy(memory.data+address,storage,size);
        else std::memcpy(storage,memory.data+address,size);
        return Stop::budget;
    }
private:
    ApplicationSettings settings_;
};
}
