#include <nt/ee_float.hpp>
// Fixed-width C ABI keeps the LLVM caller independent of C++ struct-return rules.
extern "C" std::uint64_t nt_ee_add_finite(std::uint32_t left,std::uint32_t right,std::uint32_t subtract) {
    const auto result=nt::numeric::add_subtract_finite(left,right,subtract!=0);
    return result.bits | (std::uint64_t(result.underflow)<<32) | (std::uint64_t(!result.supported)<<33);
}
