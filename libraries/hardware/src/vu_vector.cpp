#include <nt/vu_control.hpp>
#include <nt/ee_vector_math.hpp>

namespace nt::hardware {
namespace {
std::uint32_t lane(runtime::Register value, unsigned index) {
    return static_cast<std::uint32_t>((index < 2 ? value.lo : value.hi) >> ((index%2)*32));
}
}
runtime::Register VuControl::vector(unsigned reg) const {
    if (reg >= vectors_.size()) throw runtime::MemoryFault("VU: invalid vector register");
    return reg ? vectors_[reg] : runtime::Register{0,0x3f80000000000000ULL};
}
void VuControl::set_vector(unsigned reg, runtime::Register value) {
    if (reg >= vectors_.size()) throw runtime::MemoryFault("VU: invalid vector register");
    if (reg) vectors_[reg] = value;
}
void VuControl::add_subtract(unsigned destination, unsigned left, unsigned right, unsigned mask, bool subtract, unsigned broadcast) {
    const auto unpack=[](runtime::Register value) {
        return numeric::Vector{lane(value,0),lane(value,1),lane(value,2),lane(value,3)};
    };
    const auto result=numeric::add_subtract_vector(unpack(vector(destination)),unpack(vector(left)),
        unpack(vector(right)),mask,subtract,broadcast,status_);
    if (!result.supported) throw runtime::MemoryFault("VU: unsupported vector arithmetic operands or controls");
    set_vector(destination,{std::uint64_t(result.value[0])|(std::uint64_t(result.value[1])<<32),
                            std::uint64_t(result.value[2])|(std::uint64_t(result.value[3])<<32)});
    mac_=result.mac; status_=result.status;
}
}
