#pragma once
#include <array>
#include <cstdint>
#include <optional>

namespace nt::iop {
// Internal-clock timer bank. tick returns interrupt-controller cause bits.
class Timers {
public:
    std::optional<std::uint32_t> read(std::uint32_t address,unsigned bytes);
    bool write(std::uint32_t address,unsigned bytes,std::uint32_t value);
    std::uint32_t tick();
private:
    struct Counter {
        std::uint32_t count=0,target=0,mode=0x400;
        unsigned phase=0;
        bool target_pending=true;
        std::uint32_t irq_mode=0;
    };
    std::array<Counter,6> counters_{};
    static void rearm(Counter& counter);
    static bool event(Counter& counter,bool overflow);
};
}
