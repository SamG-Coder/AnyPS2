#pragma once
#include <nt/runtime.hpp>
#include <nt/vif.hpp>
#include <nt/gif.hpp>

namespace nt::hardware {
// Bounded source DMA transport shared by VIF0, VIF1 and GIF path 3.
class GraphicsDma {
public:
    static bool maps(std::uint32_t address) {
        return address >= 0x10008000 && address < 0x1000b000;
    }
    std::uint32_t read(std::uint32_t address, unsigned size) const {
        const auto reg = index(address,size);
        return channels_[(address-0x10008000)/0x1000][reg];
    }
    void write(std::uint32_t address, unsigned size, std::uint32_t value) {
        const auto reg = index(address,size);
        const auto unit = (address-0x10008000)/0x1000;
        auto& channel = channels_[unit];
        if (channel[0] & 0x100)
            throw runtime::MemoryFault("Graphics DMA: active register writes are not implemented");
        if (reg == 0 && (value & 0x100)) {
            if (!(value & 1) || (value & 0xc) > 4 || (value & 0x30) || (unit == 2 && (value & 0x40)))
                throw runtime::MemoryFault("Graphics DMA: unsupported channel, direction or mode");
            end_[unit] = (value & 0xc) == 0;
        }
        if (reg == 6 && value)
            throw runtime::MemoryFault("Graphics DMA: nonzero scratchpad setup is not implemented");
        channels_[(address-0x10008000)/0x1000][reg] = reg == 2 ? value & 0xffff : value;
    }
    std::uint32_t step(runtime::RamBus& ram, Vif& vif, Gif& gif, Gs& gs, bool enabled);
private:
    std::array<std::array<std::uint32_t,7>,3> channels_{};
    std::array<bool,3> end_{};
    static unsigned index(std::uint32_t address, unsigned size) {
        if (!maps(address) || size != 4 || (address & 15))
            throw runtime::MemoryFault("Graphics DMA: unsupported register access width or alignment");
        const auto offset = address & 0xfff;
        if (offset == 0x80) return 6; // Public DMA initialization clears this slot.
        if (offset > 0x50) throw runtime::MemoryFault("Graphics DMA: unknown register");
        return offset/16;
    }
};
}
