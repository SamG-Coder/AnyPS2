#include <nt/vu_control.hpp>

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
    const auto original = vector(destination);
    runtime::Register result{};
    const auto a = vector(left), b = vector(right);
    if (mask > 15) throw runtime::MemoryFault("VU: invalid vector lane mask");
    if (broadcast > 4) throw runtime::MemoryFault("VU: invalid broadcast lane");
    std::uint32_t flags = 0;
    for (unsigned i=0;i<4;++i) {
        const auto bit = 1U << (3-i);
        if (!(mask & bit)) continue;
        const auto value = runtime::add_subtract_finite(lane(a,i),lane(b,broadcast < 4 ? broadcast : i),subtract);
        auto& half = i < 2 ? result.lo : result.hi;
        const auto shift = (i%2)*32;
        half = (half & ~(0xffffffffULL << shift)) | (std::uint64_t(value.bits) << shift);
        if (!(value.bits & 0x7fffffff)) flags |= bit;
        if (value.bits >> 31) flags |= bit << 4;
        if (value.underflow) flags |= bit << 8;
    }
    // Commit together: unsupported operands never partially update lanes/flags.
    set_vector(destination,runtime::masked_vector(original,result,mask));
    mac_ = flags;
    const auto current = ((flags & 15) ? 1U : 0U) | ((flags & 0xf0) ? 2U : 0U) |
                         ((flags & 0xf00) ? 4U : 0U);
    status_ = (status_ & 0xff0) | current | (current << 6);
}
}
