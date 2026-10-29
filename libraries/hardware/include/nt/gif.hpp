#pragma once
#include <nt/runtime.hpp>
#include <nt/gs.hpp>

namespace nt::hardware {
// Serialized path-2/path-3 receiver sharing the GS packet decoder.
class Gif {
public:
    static bool maps(std::uint32_t address) {
        return (address >= 0x10003000 && address < 0x10003800) ||
               (address >= 0x10006000 && address < 0x10007000);
    }
    std::uint32_t read(std::uint32_t address, unsigned size) const;
    void write(std::uint32_t address, unsigned size, std::uint32_t value);
    void consume(runtime::Register value, Gs& gs, unsigned path = 3);
    bool accepts(unsigned path) const {
        return !(control_ & 8) && !(path == 3 && (mode_ & 1)) &&
               (!packet_open_ || path_ == path);
    }
    bool stopped() const { return (control_ & 8) || (mode_ & 1); }
    bool busy(unsigned path) const { return remaining_ && path_ == path; }
private:
    std::uint32_t control_ = 0, mode_ = 0;
    runtime::Register tag_{};
    unsigned remaining_ = 0, registers_ = 0, cursor_ = 0, format_ = 0;
    unsigned path_ = 0;
    bool packet_open_ = false;
    static unsigned offset(std::uint32_t address, unsigned size);
};
}
