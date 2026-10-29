#pragma once
#include <nt/runtime.hpp>

namespace nt::hardware {
// Shared VIF0/VIF1 initialization interface. FIFO and DMA execution are separate
// milestones; accepting configuration must not imply accepting their payloads.
class Vif {
public:
    static bool maps(std::uint32_t address) {
        return address >= 0x10003800 && address < 0x10006000;
    }
    std::uint32_t read(std::uint32_t address, unsigned size) const;
    void write(std::uint32_t address, unsigned size, std::uint32_t value);
private:
    struct Unit { std::array<std::uint32_t,24> registers{}; };
    std::array<Unit,2> units_{};
    static unsigned reg_index(std::uint32_t address, unsigned size);
};
}
