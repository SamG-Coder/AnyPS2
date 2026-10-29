#include <nt/sif.hpp>
#include <algorithm>

namespace nt::hardware {
namespace {
constexpr unsigned fifo_words = 128;
constexpr std::uint32_t start = 0x100, chain = 4;
void require(bool condition, const char* message) {
    if (!condition) throw runtime::MemoryFault(message);
}
void range(std::uint32_t address, std::uint32_t bytes, std::size_t capacity) {
    require(address % 4 == 0 && address <= capacity && bytes <= capacity - address,
            "SIF: invalid DMA memory extent");
}
void pop(std::deque<std::uint32_t>& fifo, unsigned count) {
    while (count--) fifo.pop_front();
}
std::uint32_t word(std::span<const std::uint8_t> memory, std::uint32_t address) {
    std::uint32_t value = 0;
    for (unsigned byte = 0; byte < 4; ++byte) value |= std::uint32_t(memory[address + byte]) << (byte * 8);
    return value;
}
}
Sif::Sif() : iop_ram_(2 * 1024 * 1024) {}
bool Sif::maps(std::uint32_t a) {
    return (a >= 0x1000c000 && a < 0x1000c040) ||
           (a >= 0x1000c400 && a < 0x1000c440) ||
           (a >= 0x1000f200 && a <= 0x1000f240);
}
std::uint32_t Sif::read(std::uint32_t a) const {
    if (a >= 0x1000f200) {
        if (a == 0x1000f240) return control_ | 0xf0000102U;
        require((a & 15) == 0 && a <= 0x1000f230,"SIF: unknown register");
        return registers_[(a - 0x1000f200) / 16];
    }
    const auto& c = a < 0x1000c400 ? receive_ : send_;
    switch (a & 0x3ff) {
    case 0: return c.control;
    case 0x10: return c.address;
    case 0x20: return c.count;
    case 0x30: if (a >= 0x1000c400) return c.tag; break;
    }
    throw runtime::MemoryFault("SIF: unknown DMA register");
}
void Sif::write(std::uint32_t a, std::uint32_t value) {
    if (a >= 0x1000f200) {
        switch (a) {
        case 0x1000f200: registers_[0] = value; return;
        case 0x1000f210: throw runtime::MemoryFault("SIF: EE cannot write IOP command address");
        case 0x1000f220: registers_[2] |= value; return;
        case 0x1000f230: registers_[3] &= ~value; return;
        case 0x1000f240:
            require(!(value & 0xc0000),"SIF: IOP interrupt/reset is not implemented");
            control_ = (control_ & ~0x100U) | (value & 0x100); return;
        default: throw runtime::MemoryFault("SIF: unknown register");
        }
    }
    const bool inbound = a < 0x1000c400;
    auto& c = inbound ? receive_ : send_;
    const auto offset = a & 0x3ff;
    if (offset == 0) {
        if (value & start) {
            require(inbound || job_.empty(),"SIF1: HLE DMA job owns the transmitter");
            require(!(c.control & start),"SIF: active DMA restart is unsupported");
            require((value & 0xc) == 0 || (value & 0xc) == chain,"SIF: unsupported DMA mode");
            require(!(value & 0x32),"SIF: interleave/stack DMA mode is unsupported");
            require((value & 1) == (inbound ? 0U : 1U),"SIF: invalid DMA direction");
            require(!inbound || !(value & 0x40),"SIF0: tag transfer enable is unsupported");
            require(!c.paused || (value & 0xc) == (c.control & 0xc),"SIF: changing mode with a paused DMA payload");
            if (!c.paused) c.end = (value & 0xc) == 0;
            c.paused = false;
        } else if (c.control & start) {
            c.paused = true;
        }
        c.control = value;
        return;
    }
    require(!(c.control & start),"SIF: modifying an active DMA channel is unsupported");
    switch (offset) {
    case 0x10: c.address = value; c.paused = false; return;
    case 0x20: c.count = value & 65535; c.paused = false; return;
    case 0x30: if (!inbound) { c.tag = value; c.paused = false; return; } break;
    }
    throw runtime::MemoryFault("SIF: unknown DMA register");
}
void Sif::receive_step(runtime::RamBus& ee) {
    auto& c = receive_;
    if (!(c.control & start)) return;
    if (!c.count) {
        if (c.end) { c.control &= ~start; completion_ |= 1U << 5; return; }
        if (to_ee_.size() < 4) return;
        const auto tag = to_ee_[0], address = to_ee_[1];
        const auto id = (tag >> 28) & 7, count = tag & 65535;
        require(id == 1 || id == 7,"SIF0: unsupported destination tag");
        require(address % 16 == 0,"SIF0: unaligned destination");
        range(address,count * 16,32 * 1024 * 1024);
        c.address = address; c.count = count;
        c.control = (c.control & 65535) | (tag & 0xffff0000U);
        c.end = id == 7 || ((c.control & 0x80) && (tag & 0x80000000U));
        pop(to_ee_,4);
        return;
    }
    if (to_ee_.size() < 4) return;
    require(c.address % 16 == 0,"SIF0: unaligned destination");
    range(c.address,16,32 * 1024 * 1024);
    ee.store_quad(c.address,{std::uint64_t(to_ee_[0]) | (std::uint64_t(to_ee_[1]) << 32),
                             std::uint64_t(to_ee_[2]) | (std::uint64_t(to_ee_[3]) << 32)});
    pop(to_ee_,4); c.address += 16; --c.count;
}
void Sif::send_step(runtime::RamBus& ee) {
    auto& c = send_;
    if (!(c.control & start)) return;
    if (!c.count) {
        if (c.end) { c.control &= ~start; completion_ |= 1U << 6; return; }
        if ((c.control & 0x40) && to_iop_.size() > fifo_words - 2) return;
        require(c.tag % 16 == 0,"SIF1: unaligned source tag");
        range(c.tag,16,32 * 1024 * 1024);
        const auto tag = ee.load_quad(c.tag);
        const auto bits = static_cast<std::uint32_t>(tag.lo), address = static_cast<std::uint32_t>(tag.lo >> 32);
        const auto id = (bits >> 28) & 7, count = bits & 65535;
        require(id <= 3 || id == 7,"SIF1: unsupported source tag");
        const auto source = id == 0 || id == 3 ? address : c.tag + 16;
        require(source % 16 == 0,"SIF1: unaligned source");
        range(source,count * 16,32 * 1024 * 1024);
        const auto next = id == 2 ? address : (id == 0 || id == 3 ? c.tag + 16 : source + count * 16);
        if (c.control & 0x40) { to_iop_.push_back(static_cast<std::uint32_t>(tag.hi)); to_iop_.push_back(static_cast<std::uint32_t>(tag.hi >> 32)); }
        c.address = source; c.count = count; c.tag = next;
        c.control = (c.control & 65535) | (bits & 0xffff0000U);
        c.end = id == 0 || id == 7 || ((c.control & 0x80) && (bits & 0x80000000U));
        return;
    }
    if (to_iop_.size() > fifo_words - 4) return;
    require(c.address % 16 == 0,"SIF1: unaligned source");
    range(c.address,16,32 * 1024 * 1024);
    const auto data = ee.load_quad(c.address);
    for (auto half : {data.lo,data.hi}) {
        to_iop_.push_back(static_cast<std::uint32_t>(half));
        to_iop_.push_back(static_cast<std::uint32_t>(half >> 32));
    }
    c.address += 16; --c.count;
}
void Sif::iop_step(runtime::RamBus& ee) {
    if (reboot_running_) {
        if (!reboot_failure_.empty()) throw runtime::MemoryFault(reboot_failure_);
        try {
            if (reboot_->receive_sif1(to_iop_)) ++received_packets_;
        } catch (const std::exception& error) {
            reboot_failure_="SIF reset: "+std::string(error.what());
            throw runtime::MemoryFault(reboot_failure_);
        }
        return;
    }
    reply_step();
    if (transmit_.active && to_ee_.size() <= fifo_words - 4) {
        if (!transmit_.tagged) {
            to_ee_.push_back((transmit_.end ? 0x70000000U : 0x10000000U) | transmit_.count);
            to_ee_.push_back(transmit_.destination); to_ee_.push_back(0); to_ee_.push_back(0);
            transmit_.tagged = true;
        } else if (transmit_.count) {
            for (unsigned i = 0; i < 4; ++i) to_ee_.push_back(word(iop_ram_,transmit_.source + i * 4));
            transmit_.source += 16; --transmit_.count;
        } else transmit_.active = false;
    }
    if (!iop_packet_) {
        if (to_iop_.size() < 4) return;
        const auto address = to_iop_[0] & 0xffffffU, words = to_iop_[1] & 0xfffffU;
        require(words % 4 == 0,"SIF1: unaligned IOP word count");
        range(address,words * 4,iop_ram_.size());
        iop_destination_ = address; iop_remaining_ = words; iop_packet_ = true;
        iop_packet_address_ = address; iop_packet_bytes_ = words * 4;
        pop(to_iop_,4);
    }
    if (iop_remaining_ && to_iop_.size() >= 4) {
        for (unsigned i = 0; i < 4; ++i)
            for (unsigned byte = 0; byte < 4; ++byte)
                iop_ram_[iop_destination_ + i * 4 + byte] = static_cast<std::uint8_t>(to_iop_[i] >> (byte * 8));
        pop(to_iop_,4); iop_destination_ += 16; iop_remaining_ -= 4;
    }
    if (!iop_remaining_) {
        if (commands_ready_ && iop_packet_address_ == registers_[1]) dispatch_command(ee);
        iop_packet_ = false; ++received_packets_;
    }
}
void Sif::step(runtime::RamBus& ee, bool enabled) {
    iop_step(ee);
    if (enabled) { receive_step(ee); send_step(ee); job_step(ee); }
    reset_step();
}
std::uint32_t Sif::take_completions() { const auto result = completion_; completion_ = 0; return result; }
void Sif::iop_write_register(unsigned index, std::uint32_t value) {
    require(index < 4,"SIF: invalid IOP register");
    if (index == 1) registers_[1] = value;
    else if (index == 2) registers_[2] &= ~value;
    else if (index == 3) registers_[3] |= value;
    else throw runtime::MemoryFault("SIF: IOP cannot write EE command address");
}
std::uint32_t Sif::iop_read_register(unsigned index) const {
    require(index < 4,"SIF: invalid IOP register"); return registers_[index];
}
std::optional<std::uint32_t> Sif::read_sbus(std::uint32_t offset) const {
    if (offset<=0x30 && !(offset&15)) return iop_read_register(offset/16);
    if (offset==0x40) return control_|0xf0000002U;
    if (offset==0x60) return 0;
    return {};
}
bool Sif::write_sbus(std::uint32_t offset,std::uint32_t value) {
    if (offset>=0x10 && offset<=0x30 && !(offset&15)) {
        iop_write_register(offset/16,value);
        return true;
    }
    if (offset==0x40) {
        if (value&~0xf0U) return false;
        const auto bits=value&0xf0;
        if (value&0xa0) control_=(control_&~0xf000U)|0x2000;
        control_=(control_&bits) ? control_&~bits : control_|bits;
        return true;
    }
    if (offset==0x60) return true; // This register reads zero after any write.
    return false;
}
void Sif::iop_install(std::uint32_t address, std::span<const std::uint8_t> bytes) {
    require(address <= iop_ram_.size() && bytes.size() <= iop_ram_.size() - address,"SIF: IOP input exceeds RAM");
    require(!transmit_.active,"SIF: modifying IOP RAM during transmission is unsupported");
    std::copy(bytes.begin(),bytes.end(),iop_ram_.begin() + address);
}
void Sif::iop_send(std::uint32_t source, std::uint32_t destination, std::uint32_t count) {
    require(!transmit_.active,"SIF: IOP transmit queue busy");
    require(count <= 65535 && source % 16 == 0 && destination % 16 == 0,"SIF: invalid IOP DMA request");
    range(source,count * 16,iop_ram_.size()); range(destination,count * 16,32 * 1024 * 1024);
    transmit_ = {source,destination,count,true,false};
}
}
