#pragma once
#include <nt/runtime.hpp>

namespace nt::hardware {
// Privileged registers and diagnostic scanout timing; no GIF renderer.
class Gs {
public:
    static bool maps(std::uint32_t address) {
        return address >= 0x12000000U && address < 0x12002000U;
    }
    std::uint64_t read(std::uint32_t address, unsigned size) const;
    void write(std::uint32_t address, unsigned size, std::uint64_t value);
    void set_crt(std::uint32_t interlace, std::uint32_t mode, std::uint32_t frame);
    void advance_cycles(std::uint64_t cycles);
    std::uint32_t take_interrupts() { const auto result = interrupts_; interrupts_ = 0; return result; }
private:
    std::array<std::uint64_t,15> display_{};
    std::uint32_t csr_ = 0x551b4000, imr_ = 0x7f00;
    std::uint32_t mode_ = 0, interrupts_ = 0;
    std::uint64_t phase_ = 0;
    unsigned half_line_ = 0;
    bool horizontal_half_ = false;
    void event(std::uint32_t flag);
    static unsigned offset(std::uint32_t address, unsigned size);
};
}
