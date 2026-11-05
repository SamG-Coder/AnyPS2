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
        case 18: return {VectorStatus::ready,storage.clip};
        case 20: return {VectorStatus::ready,storage.random};
        case 21: return {VectorStatus::ready,storage.immediate};
        case 22: return {VectorStatus::ready,storage.quotient};
        case 27: return {VectorStatus::ready,storage.launch_address};
        case 28: return {VectorStatus::ready,0};
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
        if (selector==16) {
            // Arithmetic owns the current flags; CTC2 replaces only the sticky
            // history. Completed native arithmetic is already ordered here.
            storage.status=(storage.status&63U)|(value&0xfc0U);
            return VectorStatus::ready;
        }
        if (selector==18) { storage.clip=value&0xffffffU; return VectorStatus::ready; }
        if (selector==20) { storage.random=(value&0x7fffffU)|0x3f800000U; return VectorStatus::ready; }
        if (selector==21) { storage.immediate=value; return VectorStatus::ready; }
        if (selector==22) { storage.quotient=value; return VectorStatus::ready; }
        if (selector==27) { storage.launch_address=static_cast<std::uint16_t>(value); return VectorStatus::ready; }
        if (selector!=28) return VectorStatus::unsupported;
        // Compiled microprograms have no debug or trap break to deliver.
        return value&0x0c0c ? VectorStatus::unsupported : VectorStatus::ready;
    }
private:
    static VectorReadResult read_control(void* context,unsigned selector,bool wait) {
        return static_cast<NativeVectorOwner*>(context)->read(selector,wait);
    }
    static VectorStatus write_control(void* context,unsigned selector,bool wait,std::uint32_t value) {
        return static_cast<NativeVectorOwner*>(context)->write(selector,wait,value);
    }
};
}
