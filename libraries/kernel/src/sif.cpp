#include <nt/kernel.hpp>

namespace nt::kernel {
bool Kernel::service_sif(std::uint32_t call, runtime::State& state, runtime::Bus& memory) {
    if (call == 0x6b) {
        // No arguments and no result: stop only the EE SIF0 receiver. Queued
        // IOP data and the independent SIF1 transmitter remain intact.
        const auto control=memory.load(0x1000c000,4);
        memory.store(0x1000c000,4,control & ~0x100U);
        return true;
    }
    if (call == 0x76) {
        state.set(2,runtime::sx32(static_cast<std::uint32_t>(memory.sif_status(static_cast<std::uint32_t>(state.gpr[4].lo)))));
        return true;
    }
    if (call == 0x77) {
        const auto pointer = static_cast<std::uint32_t>(state.gpr[4].lo);
        const auto count = static_cast<std::uint32_t>(state.gpr[5].lo);
        if (!count || count > 32) return false;
        if (pointer % 4 || pointer > 0xffffffffU - count * 16)
            throw runtime::MemoryFault("SifSetDma: invalid descriptor array");
        std::vector<runtime::SifTransfer> transfers;
        for (unsigned index = 0; index < count; ++index) {
            const auto base = pointer + index * 16;
            transfers.push_back({static_cast<std::uint32_t>(memory.load(base,4)),
                static_cast<std::uint32_t>(memory.load(base + 4,4)),
                static_cast<std::uint32_t>(memory.load(base + 8,4)),
                static_cast<std::uint32_t>(memory.load(base + 12,4))});
        }
        state.set(2,memory.submit_sif(transfers));
        return true;
    }
    if (call == 0x78) {
        // HLE driver initialization/rearm. Never discard an active receive.
        const auto control = memory.load(0x1000c000,4);
        if (control & 0x100) return false;
        const auto dma_control = memory.load(0x1000e000,4);
        memory.initialize_sif();
        memory.store(0x1000c020,4,0);
        memory.store(0x1000e000,4,dma_control | 1);
        memory.store(0x1000c000,4,0x184); // Destination chain, tag IRQ termination.
        memory.store(0x1000f220,4,0x10000); // EE SIF transport is initialized.
        return true;
    }
    const auto id = static_cast<std::uint32_t>(state.gpr[4].lo);
    const auto value = static_cast<std::uint32_t>(state.gpr[5].lo);
    std::uint32_t result = 0;
    if (id >= 0x80000000U && id < 0x80000003U) {
        auto& slot = sif_system_registers_[id - 0x80000000U];
        if (call == 0x79) slot = value;
        else result = slot;
    } else if (id >= 1 && id <= 4) {
        const auto address = 0x1000f200 + (id - 1) * 16;
        if (call == 0x79) memory.store(address,4,value);
        else result = static_cast<std::uint32_t>(memory.load(address,4));
    } else return false;
    state.set(2,runtime::sx32(result));
    return true;
}
}
