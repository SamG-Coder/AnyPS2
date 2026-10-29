#include <nt/gs.hpp>
#include <algorithm>

namespace nt::hardware {
void Gs::writable_local() {
    if (local_.use_count() != 1) local_ = std::make_shared<std::vector<std::uint32_t>>(*local_);
}
unsigned Gs::pixel_word(unsigned base, unsigned pitch, unsigned x, unsigned y) {
    x &= 2047; y &= 2047;
    const auto page = (y/32)*pitch+x/64;
    const auto block = ((x>>3)&1) | (((y>>3)&1)<<1) | (((x>>4)&1)<<2) |
                       (((y>>4)&1)<<3) | (((x>>5)&1)<<4);
    const auto word = (x&1) | ((y&1)<<1) | ((x&6)<<1) | ((y&6)<<3);
    return (base*64 + page*2048 + block*64 + word) & 0xfffff;
}
void Gs::upload(runtime::Register value) {
    if (!upload_.bits || upload_.pixel >= upload_.width*upload_.height)
        throw runtime::MemoryFault("GS: IMAGE data without an active host upload");
    writable_local();
    // The format and extent are validated at TRXDIR. No operation below can
    // fail, so a beat never leaves partially written local memory on a fault.
    for (unsigned byte=0;byte<16 && upload_.pixel<upload_.width*upload_.height;++byte) {
        const auto half = byte<8 ? value.lo : value.hi;
        upload_.pending |= ((half >> ((byte%8)*8)) & 255) << upload_.pending_bits;
        upload_.pending_bits += 8;
        if (upload_.pending_bits < upload_.bits) continue;
        const auto x = upload_.x + upload_.pixel%upload_.width;
        const auto y = upload_.y + upload_.pixel/upload_.width;
        auto& target = (*local_)[pixel_word(upload_.base,upload_.pitch,x,y)];
        const auto mask = upload_.bits == 32 ? 0xffffffffU : 0xffffffU;
        target = (target & ~mask) | (static_cast<std::uint32_t>(upload_.pending) & mask);
        upload_.pending >>= upload_.bits; upload_.pending_bits -= upload_.bits;
        ++upload_.pixel;
    }
}
void Gs::write_command(unsigned reg, std::uint64_t value) {
    if (reg == 0x7f || reg == 0x0f) return;
    if (reg == 6 || reg == 7) { texture_register(reg,value); return; }
    if (reg == 4 || reg == 5 || reg == 12 || reg == 13)
        { submit_vertex(reg,value); return; }
    if (reg == 0) {
        drawing_[0] = value;
        vertex_count_ = 0; // Every PRIM write restarts primitive assembly.
        return;
    }
    if (reg == 0x54 || (reg == 0x53 && (value & 3) != 3 && (value & 3) != 0))
        throw runtime::MemoryFault("GS: image transfer direction " + std::to_string(value & 3) + " is not implemented");
    if (reg == 0x53) {
        Upload next;
        if ((value & 3) == 0) {
            const auto buffer=drawing_[0x50], position=drawing_[0x51], extent=drawing_[0x52];
            const auto format=static_cast<unsigned>((buffer>>56)&63);
            if (format != 0 && format != 1)
                throw runtime::MemoryFault("GS: upload pixel format " + std::to_string(format) + " is not implemented");
            if ((position>>59)&3) throw runtime::MemoryFault("GS: reverse upload traversal is not implemented");
            next.base=static_cast<unsigned>((buffer>>32)&0x3fff);
            next.pitch=static_cast<unsigned>((buffer>>48)&63);
            next.x=static_cast<unsigned>((position>>32)&2047); next.y=static_cast<unsigned>((position>>48)&2047);
            next.width=static_cast<unsigned>(extent&4095); next.height=static_cast<unsigned>((extent>>32)&4095);
            next.bits=format == 0 ? 32 : 24;
        }
        upload_=next; drawing_[reg]=value; return;
    }
    if (reg == 0x60) {
        if (csr_ & 1) throw runtime::MemoryFault("GS: queued SIGNAL is not implemented");
        const auto mask = static_cast<std::uint32_t>(value >> 32);
        signal_label_ = (signal_label_ & ~(std::uint64_t(mask))) | (static_cast<std::uint32_t>(value) & mask);
        event(1); return;
    }
    if (reg == 0x61) { event(2); return; }
    if (reg == 0x62) {
        const auto mask = value & 0xffffffff00000000ULL;
        signal_label_ = (signal_label_ & ~mask) | ((value << 32) & mask);
        return;
    }
    switch (reg) {
    case 0: case 1: case 2: case 3: case 6: case 7: case 8: case 9: case 10:
    case 0x14: case 0x15: case 0x16: case 0x17: case 0x18: case 0x19: case 0x1a: case 0x1b:
    case 0x1c: case 0x22: case 0x34: case 0x35: case 0x36: case 0x37:
    case 0x3b: case 0x3d: case 0x3f: case 0x40: case 0x41: case 0x42: case 0x43:
    case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4a:
    case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f: case 0x50: case 0x51:
    case 0x52: case 0x53: drawing_[reg] = value; return;
    default: throw runtime::MemoryFault("GS: unsupported command register " + std::to_string(reg));
    }
}
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
        reg == 0x1000 ? csr_ : reg == 0x1010 ? imr_ : reg == 0x1080 ? signal_label_ : 0ULL;
    const auto mask = size == 8 ? ~0ULL : (1ULL << (size*8))-1;
    return (value >> ((address & 7)*8)) & mask;
}
void Gs::write(std::uint32_t address, unsigned size, std::uint64_t value) {
    const auto reg = offset(address,size);
    const auto shift = (address & 7)*8;
    const auto mask = (size == 8 ? ~0ULL : (1ULL << (size*8))-1) << shift;
    value = (value << shift) & mask;
    if (reg == 0x1000) {
        if (value & 0x200) { auto memory=local_; *this = Gs{}; local_=std::move(memory); return; }
        csr_ &= ~static_cast<std::uint32_t>(value & 31);
        // Accepted register packets execute synchronously; no rendering queue exists.
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
