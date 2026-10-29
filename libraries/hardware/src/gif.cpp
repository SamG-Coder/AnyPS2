#include <nt/gif.hpp>
#include <algorithm>

namespace nt::hardware {
void Gif::consume(runtime::Register value, Gs& gs, unsigned path) {
    if (path != 2 && path != 3) throw runtime::MemoryFault("GIF: unsupported submission path");
    if (!accepts(path)) throw runtime::MemoryFault("GIF: submission while path is busy");
    if (remaining_ && format_ >= 2) {
        gs.upload(value);
        --remaining_;
        if (!remaining_ && (tag_.lo & 0x8000)) packet_open_ = false;
        return;
    }
    auto next = *this;
    auto result = gs; // At most two GS writes per beat; commit only after validation.
    if (!remaining_) {
        const auto loops = static_cast<unsigned>(value.lo & 0x7fff);
        const auto format = static_cast<unsigned>((value.lo >> 58) & 3);
        next.tag_ = value; next.format_ = format; next.cursor_ = 0;
        next.path_ = path;
        next.packet_open_ = true;
        next.registers_ = static_cast<unsigned>(value.lo >> 60);
        if (!next.registers_) next.registers_ = 16;
        next.remaining_ = format >= 2 ? loops : loops * next.registers_;
        if (format == 0 && (value.lo & (1ULL << 46))) result.write_command(0,(value.lo >> 47) & 0x7ff);
    } else {
        const unsigned count = format_ == 0 ? 1 : std::min(2U,remaining_);
        for (unsigned half=0;half<count;++half) {
            const auto reg = static_cast<unsigned>((tag_.hi >> (next.cursor_*4)) & 15);
            if (format_ == 0) {
                if (reg == 14) result.write_command(static_cast<unsigned>(value.hi & 127),value.lo);
                else if (reg == 0) result.write_command(0,value.lo & 0x7ff);
                else if (reg != 15) throw runtime::MemoryFault("GIF: packed register " + std::to_string(reg) + " is not implemented");
            } else {
                if (reg == 14) throw runtime::MemoryFault("GIF: A_D is invalid in REGLIST");
                if (reg != 15) result.write_command(reg,half ? value.hi : value.lo);
            }
            --next.remaining_;
            next.cursor_ = (next.cursor_+1)%registers_;
        }
    }
    if (!next.remaining_ && (next.tag_.lo & 0x8000)) next.packet_open_ = false;
    *this = next; gs = result;
}
unsigned Gif::offset(std::uint32_t address, unsigned size) {
    if (address >= 0x10006000 && address < 0x10007000)
        throw runtime::MemoryFault("GIF: FIFO packet processing is not implemented");
    if (!maps(address) || size != 4 || (address & 15))
        throw runtime::MemoryFault("GIF: unsupported register access width or alignment");
    const auto reg = address - 0x10003000;
    if (reg > 0xa0 || reg == 0x30)
        throw runtime::MemoryFault("GIF: unknown register");
    return reg;
}
std::uint32_t Gif::read(std::uint32_t address, unsigned size) const {
    switch (offset(address,size)) {
    case 0x00: return control_;
    case 0x10: return mode_;
    case 0x20: return control_ | mode_ | (remaining_ ? path_ << 10 : 0);
    case 0x40: return static_cast<std::uint32_t>(tag_.lo);
    case 0x50: return static_cast<std::uint32_t>(tag_.lo >> 32);
    case 0x60: return static_cast<std::uint32_t>(tag_.hi);
    case 0x70: return static_cast<std::uint32_t>(tag_.hi >> 32);
    default: return 0; // No tag, path or FIFO counters before packet acceptance.
    }
}
void Gif::write(std::uint32_t address, unsigned size, std::uint32_t value) {
    switch (offset(address,size)) {
    case 0x00:
        if (value & 1) { *this = Gif{}; }
        else control_ = value & 8;
        return;
    case 0x10: mode_ = value & 5; return;
    default: throw runtime::MemoryFault("GIF: writes to status/tag registers are not implemented");
    }
}
}
