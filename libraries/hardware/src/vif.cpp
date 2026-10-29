#include <nt/vif.hpp>

namespace nt::hardware {
unsigned Vif::reg_index(std::uint32_t address, unsigned size) {
    if (address >= 0x10004000 && address < 0x10006000)
        throw runtime::MemoryFault("VIF: FIFO command execution is not implemented");
    if (!maps(address) || size != 4 || (address & 15))
        throw runtime::MemoryFault("VIF: unsupported register access width or alignment");
    const auto reg = (address & 0x3ff) / 16;
    const bool vif1 = address >= 0x10003c00;
    if (reg >= 24 || reg == 15 || (!vif1 && (reg == 10 || reg == 11 || reg == 12 || reg == 14)))
        throw runtime::MemoryFault("VIF: unknown register");
    return reg;
}
std::uint32_t Vif::read(std::uint32_t address, unsigned size) const {
    const auto reg = reg_index(address,size);
    return units_[address >= 0x10003c00].registers[reg];
}
void Vif::write(std::uint32_t address, unsigned size, std::uint32_t value) {
    const auto reg = reg_index(address,size);
    const bool vif1 = address >= 0x10003c00;
    auto& r = units_[vif1].registers;
    switch (reg) {
    case 0: // VIF0 STAT is read-only; only VIF1 FDR is writable.
        if (vif1 && (value & (1U << 23)))
            throw runtime::MemoryFault("VIF1: reverse FIFO transfer is not implemented");
        return;
    case 1: // FBRST command strobes are not retained on readback.
        if (value & 2) r[0] |= 1U << 9; // Force break, already idle.
        if (value & 4) r[0] |= 1U << 8; // Stop, no command in flight.
        if (value & 8) r[0] &= ~0x3f00U; // Cancel stop, break, interrupt/error stalls.
        if (value & 1) {
            r[0] = 0;
            r[2] = 0;
            // Configuration, including row/column data, survives a soft reset.
            // No active DMA or FIFO payload can exist at this milestone.
        }
        return;
    case 2: r[reg] = value & 7; return;
    case 3: r[reg] = value & 0xffff; r[0] &= ~(1U << 6); return;
    case 5: r[reg] = value & 3; return;
    case 7: r[reg] = value; return;
    default:
        if (reg >= 16) { r[reg] = value; return; }
        throw runtime::MemoryFault("VIF: command-managed register writes are not implemented");
    }
}
}
