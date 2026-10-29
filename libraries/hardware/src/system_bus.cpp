#include <nt/system_bus.hpp>

namespace nt::hardware {
std::uint32_t SystemBus::physical(std::uint32_t a) {
    return a >= 0x80000000U && a < 0xc0000000U ? a & 0x1fffffffU : a;
}
std::uint64_t SystemBus::load(std::uint32_t a, unsigned size) {
    last_address = a; last_size = size; last_write = false;
    const auto p = physical(a);
    if (Sif::maps(p)) {
        if (size != 4) throw runtime::MemoryFault("SIF: unsupported register access width");
        return sif_.read(p);
    }
    if (size == 4 && p == 0x1000e000) return dma_control_;
    if (size == 4 && (p == 0x1000f520 || p == 0x1000f590)) return dma_enable_;
    if (size == 4 && p == 0x1000f000) return interrupt_status_;
    if (size == 4 && p == 0x1000f010) return interrupt_mask_;
    if (size == 4 && p == 0x1000e010) return dma_status_;
    if (p >= 0x10000000 && p < 0x10002000) {
        const auto index = (p - 0x10000000) / 0x800, reg = p & 0x7ff;
        if (size != 4) throw runtime::MemoryFault("Unsupported timer access width");
        const auto& timer = timers_[index];
        switch (reg) {
        case 0x00: return timer.count;
        case 0x10: return timer.mode;
        case 0x20: return timer.target;
        case 0x30: if (index < 2) return timer.hold; break;
        }
        throw runtime::MemoryFault("Unknown timer register");
    }
    return ram_.load(a,size);
}
void SystemBus::store(std::uint32_t a, unsigned size, std::uint64_t value) {
    last_address = a; last_size = size; last_write = true;
    const auto p = physical(a);
    if (Sif::maps(p)) {
        if (size != 4) throw runtime::MemoryFault("SIF: unsupported register access width");
        sif_.write(p,static_cast<std::uint32_t>(value)); return;
    }
    if (size == 4 && p == 0x1000e000) {
        if (value & ~1ULL) throw runtime::MemoryFault("DMAC: stall/MFIFO/release modes are unsupported");
        dma_control_ = static_cast<std::uint32_t>(value); return;
    }
    if (size == 4 && p == 0x1000f590) { dma_enable_ = static_cast<std::uint32_t>(value); return; }
    if (size == 4 && p == 0x1000f000) { interrupt_status_ &= ~static_cast<std::uint32_t>(value); return; }
    if (size == 4 && p == 0x1000f010) { interrupt_mask_ ^= static_cast<std::uint32_t>(value) & 65535; return; }
    if (size == 4 && p == 0x1000e010) {
        const auto v = static_cast<std::uint32_t>(value);
        dma_status_ = (dma_status_ & ~(v & 65535)) ^ (v & 0xffff0000U);
        return;
    }
    if (p >= 0x10000000 && p < 0x10002000) {
        const auto index = (p - 0x10000000) / 0x800, reg = p & 0x7ff;
        if (size != 4) throw runtime::MemoryFault("Unsupported timer access width");
        auto& timer = timers_[index];
        const auto v = static_cast<std::uint32_t>(value);
        switch (reg) {
        case 0x00: timer.count = v & 65535; return;
        case 0x10:
            if ((v & 4) || (v & 3) == 3) throw runtime::MemoryFault("Gated/HBlank timer requires video timing");
            timer.mode = (timer.mode & 0xc00 & ~v) | (v & 0x3ff);
            timer.phase = static_cast<std::uint32_t>(cycles_ % ((v & 3) == 0 ? 2 : (v & 3) == 1 ? 32 : 512));
            return;
        case 0x20: timer.target = v & 65535; return;
        case 0x30: if (index < 2) { timer.hold = v & 65535; return; } break;
        }
        throw runtime::MemoryFault("Unknown timer register");
    }
    ram_.store(a,size,value);
}
runtime::Register SystemBus::load_quad(std::uint32_t address) {
    last_address = address; last_size = 16; last_write = false;
    return ram_.load_quad(address);
}
void SystemBus::store_quad(std::uint32_t address, runtime::Register value) {
    last_address = address; last_size = 16; last_write = true;
    ram_.store_quad(address,value);
}
void SystemBus::advance_cycles(std::uint64_t cycles) {
    // One bounded transport step per diagnostic EE cycle. This is not bus timing.
    for (std::uint64_t step = 0; step < cycles; ++step)
        sif_.step(ram_,(dma_control_ & 1) && !(dma_enable_ & 0x10000));
    dma_status_ |= sif_.take_completions();
    cycles_ += cycles;
    for (unsigned index=0; index<timers_.size(); ++index) {
        auto& timer = timers_[index];
        if (!(timer.mode & 0x80)) continue;
        const auto divisor = (timer.mode & 3) == 0 ? 2U : (timer.mode & 3) == 1 ? 32U : 512U;
        const auto elapsed = cycles + timer.phase;
        auto ticks = elapsed / divisor;
        timer.phase = static_cast<std::uint32_t>(elapsed % divisor);
        while (ticks--) {
            timer.count = (timer.count + 1) & 65535;
            if (timer.count == 0 && (timer.mode & 0x200)) {
                timer.mode |= 0x800;
                interrupt_status_ |= 1U << (9+index);
            }
            if (timer.count == timer.target && (timer.mode & 0x100)) {
                timer.mode |= 0x400;
                interrupt_status_ |= 1U << (9+index);
                if (timer.mode & 0x40) timer.count = 0;
            }
        }
    }
}
}
