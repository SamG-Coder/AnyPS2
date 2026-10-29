#include <nt/kernel.hpp>
#include <algorithm>

namespace nt::kernel {
bool Kernel::service_interrupts(std::uint32_t call, runtime::State& state, runtime::Bus& memory) {
    const auto argument = [&](unsigned index) { return static_cast<std::uint32_t>(state.gpr[index+4].lo); };
    const bool dma = call == 0x12 || call == 0x13 || call == 0x16 || call == 0x17 || call == 0x1c || call == 0x1d;
    const auto line = argument(0);
    if (line >= (dma ? 10U : 16U)) { state.set(2,~std::uint64_t(0)); return true; }
    auto& records = dma ? dmac_records_ : intc_records_;
    if (call == 0x10 || call == 0x12) {
        const auto address = argument(1), next = argument(2);
        const auto physical = address & 0x1fffffffU;
        if (!(address == physical || (address >= 0x80000000U && address < 0xc0000000U)) ||
            physical < 4 || physical >= 0x2000000 || physical % 4)
            throw runtime::MemoryFault("Interrupt registration has invalid code address");
        memory.load(physical,4);
        auto position = records.begin();
        if (next == 0xffffffffU) position = records.end();
        else if (next) {
            position = std::find_if(records.begin(),records.end(),[&](const auto& r) { return r.id == next; });
            if (position == records.end()) { state.set(2,~std::uint64_t(0)); return true; }
        }
        std::uint32_t id = 1;
        while (std::any_of(records.begin(),records.end(),[&](const auto& r) { return r.id == id; })) ++id;
        if (id > 64) { state.set(2,~std::uint64_t(0)); return true; }
        records.insert(position,{id,line,physical,argument(3),static_cast<std::uint32_t>(state.gpr[28].lo)});
        state.set(2,id);
        return true;
    }
    if (call == 0x11 || call == 0x13) {
        const auto id = argument(1);
        const auto found = std::find_if(records.begin(),records.end(),[&](const auto& r) { return r.id == id && r.line == line; });
        if (found == records.end()) { state.set(2,~std::uint64_t(0)); return true; }
        records.erase(found);
        state.set(2,static_cast<std::uint64_t>(std::count_if(records.begin(),records.end(),[&](const auto& r) { return r.line == line; })));
        return true;
    }
    const bool enable = call == 0x14 || call == 0x16 || call == 0x1a || call == 0x1c;
    const auto address = dma ? 0x1000e010U : 0x1000f010U;
    const auto mask = 1U << (line + (dma ? 16 : 0));
    const bool current = (memory.load(address,4) & mask) != 0;
    if (current != enable) memory.store(address,4,mask);
    state.set(2,current != enable ? 1 : 0);
    return true;
}
}
