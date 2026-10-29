#include <nt/gs.hpp>
#include <algorithm>

namespace nt::hardware {
void Gs::set_crt(std::uint32_t interlace, std::uint32_t mode, std::uint32_t frame) {
    if (interlace != 1 || (mode != 2 && mode != 3) || frame > 1)
        throw runtime::MemoryFault("GS: SetGsCrt currently supports interlaced NTSC/PAL only");
    mode_ = mode;
    display_[2] = 1 | (frame << 1); // SMODE2 INT/FFMD, normal power mode.
    phase_ = 0;
    half_line_ = 0;
    horizontal_half_ = false;
    csr_ &= ~0x3000U;
}
void Gs::event(std::uint32_t flag) {
    if (!(csr_ & flag) && !(imr_ & (flag << 8))) interrupts_ |= 1;
    csr_ |= flag;
}
void Gs::advance_cycles(std::uint64_t cycles) {
    if (!mode_) return;
    // Rational half-line clocks preserve fractional cycles across calls.
    // EE execution currently charges one diagnostic cycle per instruction.
    const std::uint64_t rate = mode_ == 2 ? 31500000 : 31250;
    const std::uint64_t period = 294912000ULL * (mode_ == 2 ? 1001 : 1);
    const unsigned lines = mode_ == 2 ? 525 : 625;
    const unsigned blank = mode_ == 2 ? 480 : 576;
    const unsigned field = blank + (mode_ == 2 ? 7 : 6);
    while (cycles) {
        const auto step = std::min(cycles,(period-phase_+rate-1)/rate);
        phase_ += step*rate;
        cycles -= step;
        if (phase_ < period) break;
        phase_ -= period;
        ++half_line_;
        horizontal_half_ = !horizontal_half_;
        if (!horizontal_half_) event(4); // Diagnostic horizontal sync edge.
        if (half_line_ == blank) interrupts_ |= 1U << 2;
        if (half_line_ == field) { csr_ ^= 0x2000; event(8); }
        if (half_line_ == lines) { half_line_ = 0; interrupts_ |= 1U << 3; }
    }
}
unsigned Gs::offset(std::uint32_t address, unsigned size) {
    if (!maps(address) || (size != 1 && size != 2 && size != 4 && size != 8) ||
        (address & (size-1)) || (address & 15) + size > 8)
        throw runtime::MemoryFault("GS: unsupported register access width or alignment");
    const auto reg = (address - 0x12000000U) & ~15U;
    if (reg > 0xe0 && reg != 0x1000 && reg != 0x1010 && reg != 0x1040 && reg != 0x1080)
        throw runtime::MemoryFault("GS: unknown privileged register");
    return reg;
}
std::uint64_t Gs::read(std::uint32_t address, unsigned size) const {
    const auto reg = offset(address,size);
    const auto value = reg <= 0xe0 ? display_[reg/16] :
        reg == 0x1000 ? csr_ : reg == 0x1010 ? imr_ : 0ULL;
    const auto mask = size == 8 ? ~0ULL : (1ULL << (size*8))-1;
    return (value >> ((address & 7)*8)) & mask;
}
void Gs::write(std::uint32_t address, unsigned size, std::uint64_t value) {
    const auto reg = offset(address,size);
    const auto shift = (address & 7)*8;
    const auto mask = (size == 8 ? ~0ULL : (1ULL << (size*8))-1) << shift;
    value = (value << shift) & mask;
    if (reg == 0x1000) {
        if (value & 0x200) { *this = Gs{}; return; }
        csr_ &= ~static_cast<std::uint32_t>(value & 31);
        // FLUSH has no outstanding work: GIF submissions are not yet accepted.
        return;
    }
    if (reg == 0x1010) {
        const auto next = static_cast<std::uint32_t>(((imr_ & ~mask) | value) & 0x1f00) | 0x6000;
        if ((csr_ & 31) & (imr_ >> 8) & ~(next >> 8)) interrupts_ |= 1;
        imr_ = next;
        return;
    }
    if (reg == 0x1040) {
        if (value & 1) throw runtime::MemoryFault("GS: local-to-host transfer is not implemented");
        return;
    }
    if (reg == 0x1080) throw runtime::MemoryFault("GS: SIGLBLID writes are not implemented");
    const auto next = (display_[reg/16] & ~mask) | value;
    if (mode_ && reg >= 0x10 && reg <= 0x60 && next != display_[reg/16] &&
        !(reg == 0x20 && (next & ~2ULL) == 1))
        throw runtime::MemoryFault("GS: direct scanout timing changes are not implemented");
    if (reg == 0xd0 && (next & 1))
        throw runtime::MemoryFault("GS: external feedback transfer is not implemented");
    display_[reg/16] = next;
}
}
