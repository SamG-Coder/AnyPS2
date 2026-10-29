#pragma once
#include <nt/ee_native.hpp>

namespace nt::ee {
// Completion events raised by native transfer producers between execution slices.
// No channel engine, transfer timing, or synthetic completion is implemented here.
class NativeCompletions {
public:
    bool complete(unsigned channel) {
        if (channel>=10) return false;
        pending_|=1U<<channel;
        return true;
    }
    std::uint32_t ready() const { return pending_&enabled_&0x3ffU; }
    bool enable(unsigned channel,bool value) {
        if (channel>=10) return false;
        const auto mask=1U<<channel; const bool previous=(enabled_&mask)!=0;
        if (value) enabled_|=mask; else enabled_&=~mask;
        return previous!=value;
    }
    std::uint32_t status() const { return pending_|(enabled_<<16); }
    void acknowledge(std::uint32_t value) {
        pending_&=~(value&0xffffU);
        enabled_^=value>>16;
    }
    void publish(Memory& memory) const {
        memory.pending_interrupts=(memory.pending_interrupts&~0x800U)|
            ((pending_&enabled_&0x3ffU) ? 0x800U : 0U);
    }
private:
    std::uint32_t pending_=0,enabled_=0;
};
}
