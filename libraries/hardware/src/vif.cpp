#include <nt/vif.hpp>
#include <nt/gif.hpp>

namespace nt::hardware {
bool Vif::consume(unsigned unit, std::span<const std::uint32_t> words, Gif* gif, Gs* gs) {
    if (unit > 1 || words.size() > 4)
        throw runtime::MemoryFault("VIF: invalid unit or oversized DMA beat");
    // Validate and commit a DMA beat together; unsupported commands cannot lose
    // preceding words when a diagnostic run is resumed.
    auto next = units_.at(unit);
    std::array<std::pair<unsigned,std::uint32_t>,4> uploads{};
    unsigned upload_count = 0;
    runtime::Register packet{};
    bool submit = false;
    auto& r = next.registers;
    for (const auto word : words) {
        if (next.remaining) {
            if (next.command == 0x50) {
                if (!gif || !gs) throw runtime::MemoryFault("VIF: DIRECT requires a GIF receiver");
                next.direct[next.cursor++] = word;
                if (next.cursor == 4) {
                    packet = {std::uint64_t(next.direct[0]) | (std::uint64_t(next.direct[1])<<32),
                              std::uint64_t(next.direct[2]) | (std::uint64_t(next.direct[3])<<32)};
                    submit = true;
                    next.cursor = 0;
                }
            } else if (next.command == 0x4a) {
                uploads[upload_count++] = {next.cursor++ % (unit ? 4096 : 1024),word};
            } else if (next.command == 0x20) r[7] = word;
            else r[(next.command == 0x30 ? 16 : 20) + next.cursor++] = word;
            --next.remaining;
            r[6] = next.command == 0x4a ? ((next.remaining+1)/2)&255 : 0;
            if (!next.remaining) { next.command = 0; r[0] &= ~3U; }
            continue;
        }
        const unsigned command = (word >> 24) & 127;
        if (word & 0x80000000U)
            throw runtime::MemoryFault("VIF: command interrupts are not implemented");
        if (!unit && (command == 2 || command == 3 || command == 0x11 || command == 0x50))
            throw runtime::MemoryFault("VIF0: VIF1-only command is invalid");
        switch (command) {
        case 0: break;
        case 1: r[4] = word & 65535; break; // STCYCL
        case 2: // OFFSET resets double buffering to BASE.
            if (unit) {
                r[11] = word & 1023;
                r[12] = r[10];
                r[0] &= ~(1U << 7);
            }
            break;
        case 3: if (unit) r[10] = word & 1023; break; // BASE (VIF1 only)
        case 4: r[9] = word & 1023; break; // ITOP stages ITOPS until VU launch.
        case 5: r[5] = word & 3; break; // STMOD
        case 7: r[3] = word & 65535; r[0] |= 1U << 6; break;
        case 0x10: break; // VU execution is rejected before activation.
        case 0x11:
            if (submit) throw runtime::MemoryFault("VIF: FLUSH sharing a final DIRECT beat is not implemented");
            if (gif && gif->busy(2)) return false;
            break; // Path 1 is unsupported; FLUSHA also waits for path 3.
        case 0x20: next.remaining = 1; break;
        case 0x30: case 0x31: next.remaining = 4; break;
        case 0x4a:
            next.remaining = ((word >> 16) & 255) ? ((word >> 16) & 255)*2 : 512;
            break;
        case 0x50:
            if (!gif || !gs) throw runtime::MemoryFault("VIF: DIRECT requires a GIF receiver");
            next.remaining = (word & 65535) ? (word & 65535)*4 : 65536*4;
            break;
        default: throw runtime::MemoryFault("VIF: command " + std::to_string(command) + " is not implemented");
        }
        r[8] = word;
        if (next.remaining) {
            next.command = command;
            next.cursor = command == 0x4a ? (word & 65535)*2 : 0;
            r[6] = command == 0x4a ? (next.remaining/2)&255 : 0;
            r[0] = (r[0] & ~3U) | 1U;
        }
    }
    if (submit) {
        if (!gif->accepts(2)) return false;
        gif->consume(packet,*gs,2);
    }
    units_[unit] = next;
    for (unsigned i=0;i<upload_count;++i) micro_[unit][uploads[i].first] = uploads[i].second;
    return true;
}
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
            units_[vif1].remaining = units_[vif1].command = units_[vif1].cursor = 0;
            r[0] = 0;
            r[2] = 0;
            // Configuration, including row/column data, survives a soft reset.
            // Discard pending command payload without erasing uploaded VU code.
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
