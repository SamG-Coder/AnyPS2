#pragma once
#include <nt/ee_vector.hpp>

namespace nt::ee {
// Cooperative owner for EE vector access. Completed compiled VU0 launches publish
// through the same storage. Reset of active native work remains unsupported.
class NativeVectorOwner {
public:
    VectorStorage storage;
    VectorControl interface() { return {this,read_control,write_control}; }
    VectorReadResult read(unsigned selector,bool wait) const {
        if (storage.active && (wait || selector!=29)) return {VectorStatus::pending};
        if (selector<16) return {VectorStatus::ready,selector ? storage.integers[selector-1] : 0U};
        switch (selector) {
        case 16: return {VectorStatus::ready,storage.status};
        case 17: return {VectorStatus::ready,storage.mac};
        case 27: return {VectorStatus::ready,storage.launch_address};
        case 28: return {VectorStatus::ready,break_control_};
        case 29: return {VectorStatus::ready,storage.active ? 1U : 0U};
        default: return {VectorStatus::unsupported};
        }
    }
    VectorStatus write(unsigned selector,bool,std::uint32_t value) {
        if (storage.active) return VectorStatus::pending;
        if (selector<16) {
            if (selector) storage.integers[selector-1]=static_cast<std::uint16_t>(value);
            return VectorStatus::ready;
        }
        if (selector==17 || selector==29) return VectorStatus::ready;
        if (selector==27) { storage.launch_address=static_cast<std::uint16_t>(value); return VectorStatus::ready; }
        if (selector!=28) return VectorStatus::unsupported;
        break_control_=value&0x0c0c;
        if (value&2) break_control_&=~0xffU;
        if (value&0x200) break_control_&=~0xff00U;
        return VectorStatus::ready;
    }
private:
    static VectorReadResult read_control(void* context,unsigned selector,bool wait) {
        return static_cast<NativeVectorOwner*>(context)->read(selector,wait);
    }
    static VectorStatus write_control(void* context,unsigned selector,bool wait,std::uint32_t value) {
        return static_cast<NativeVectorOwner*>(context)->write(selector,wait,value);
    }
    std::uint32_t break_control_=0;
};
}
