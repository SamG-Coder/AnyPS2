#include <nt/ee_float.hpp>
namespace {
std::uint64_t pack(nt::numeric::FiniteFloatResult result) {
    return result.bits | (std::uint64_t(result.underflow)<<32) | (std::uint64_t(!result.supported)<<33);
}
std::uint64_t pack_flags(nt::numeric::ScalarFlagResult result) {
    return result.bits | (std::uint64_t(result.flags)<<32) | (std::uint64_t(!result.supported)<<63);
}
}
// Fixed-width C ABI keeps the LLVM caller independent of C++ struct-return rules.
extern "C" std::uint64_t nt_ee_add_finite(std::uint32_t left,std::uint32_t right,std::uint32_t subtract) {
    return pack(nt::numeric::add_subtract_finite(left,right,subtract!=0));
}

extern "C" std::uint64_t nt_ee_multiply_finite(std::uint32_t left,std::uint32_t right) {
    return pack(nt::numeric::multiply_finite(left,right));
}

extern "C" std::uint64_t nt_ee_divide_finite(std::uint32_t left,std::uint32_t right) {
    return pack_flags(nt::numeric::divide_finite(left,right));
}
extern "C" std::uint32_t nt_ee_integer_to_scalar(std::uint32_t value) {
    return nt::numeric::integer_to_scalar(value);
}
extern "C" std::uint32_t nt_ee_scalar_to_integer(std::uint32_t value) {
    return nt::numeric::scalar_to_integer(value);
}
extern "C" std::uint32_t nt_ee_min_max_scalar(std::uint32_t left,std::uint32_t right,std::uint32_t maximum) {
    return nt::numeric::min_max_scalar(left,right,maximum!=0);
}

extern "C" std::uint32_t nt_ee_integer_to_scaled_scalar(std::uint32_t value,std::uint32_t scale) {
    return nt::numeric::integer_to_scaled_scalar(value,scale);
}
extern "C" std::uint32_t nt_ee_scaled_scalar_to_integer(std::uint32_t value,std::uint32_t scale) {
    return nt::numeric::scaled_scalar_to_integer(value,scale);
}

extern "C" std::uint64_t nt_ee_square_root_finite(std::uint32_t value) {
    return pack_flags(nt::numeric::square_root_finite(value));
}
