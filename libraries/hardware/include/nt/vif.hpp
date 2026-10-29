#pragma once
#include <nt/runtime.hpp>

namespace nt::hardware {
class Gif;
class Gs;
// Shared VIF0/VIF1 configuration and bounded DMA command receiver.
class Vif {
public:
    static bool maps(std::uint32_t address) {
        return address >= 0x10003800 && address < 0x10006000;
    }
    std::uint32_t read(std::uint32_t address, unsigned size) const;
    void write(std::uint32_t address, unsigned size, std::uint32_t value);
    bool consume(unsigned unit, std::span<const std::uint32_t> words, Gif* gif = nullptr, Gs* gs = nullptr);
    bool stopped(unsigned unit) const { return (units_.at(unit).registers[0] & 0x300) != 0; }
    std::uint32_t micro_word(unsigned unit, unsigned word) const { return micro_.at(unit).at(word); }
private:
    struct Unit {
        std::array<std::uint32_t,24> registers{};
        unsigned command = 0, remaining = 0, cursor = 0;
        std::array<std::uint32_t,4> direct{};
    };
    std::array<Unit,2> units_{};
    std::array<std::array<std::uint32_t,4096>,2> micro_{};
    static unsigned reg_index(std::uint32_t address, unsigned size);
};
}
