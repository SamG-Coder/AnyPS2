#include <nt/ee_float.hpp>
#include <nt/ee_vector_math.hpp>
#include <nt/vu_quotient.hpp>
#include <nt/vu_extended.hpp>
#include <nt/vu_clip.hpp>
namespace {
std::uint64_t pack(nt::numeric::FiniteFloatResult result) {
    return result.bits | (std::uint64_t(result.underflow)<<32) | (std::uint64_t(!result.supported)<<33);
}
std::uint64_t pack_flags(nt::numeric::ScalarFlagResult result) {
    return result.bits | (std::uint64_t(result.flags)<<32) | (std::uint64_t(!result.supported)<<63);
}
}
// Fixed-width C ABI keeps the LLVM caller independent of C++ struct-return rules.
extern "C" std::uint32_t nt_vu_clip_flags(std::uint32_t x,std::uint32_t y,std::uint32_t z,std::uint32_t w) {
    return nt::numeric::clip_flags(x,y,z,w);
}
extern "C" std::uint64_t nt_vu_reciprocal_length_finite(std::uint32_t x,std::uint32_t y,std::uint32_t z) {
    return pack(nt::numeric::reciprocal_length_finite(x,y,z));
}
extern "C" std::uint64_t nt_vu_divide_finite(std::uint32_t left,std::uint32_t right) {
    return pack_flags(nt::numeric::vu_divide_finite(left,right));
}
extern "C" std::uint64_t nt_vu_square_root_finite(std::uint32_t right) {
    return pack_flags(nt::numeric::vu_square_root_finite(right));
}
extern "C" std::uint64_t nt_vu_reciprocal_square_root_finite(std::uint32_t left,std::uint32_t right) {
    return pack_flags(nt::numeric::vu_reciprocal_square_root_finite(left,right));
}
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

extern "C" std::uint32_t nt_ee_vector_lane_flags(std::uint64_t packed,std::uint32_t lane) {
    return lane<4 ? nt::numeric::vector_lane_flags(static_cast<std::uint32_t>(packed),((packed>>32)&1)!=0,lane) : 0;
}
extern "C" std::uint32_t nt_ee_vector_status_flags(std::uint32_t mac,std::uint32_t previous) {
    return nt::numeric::vector_status_flags(mac,previous);
}

extern "C" std::uint64_t nt_ee_multiply_accumulate_finite(std::uint32_t accumulator,std::uint32_t left,
                                                       std::uint32_t right,std::uint32_t subtract) {
    return pack(nt::numeric::multiply_accumulate_finite(accumulator,left,right,subtract!=0));
}

extern "C" std::uint64_t nt_ee_reciprocal_square_root_finite(std::uint32_t left,std::uint32_t right) {
    // Preserve the finite-domain contract. Exceptional denominator semantics
    // require separate validation rather than a fabricated quotient.
    if ((right>>31) || !(right&0x7f800000U)) return pack_flags({0,0,false});
    const auto root=nt::numeric::square_root_finite(right);
    if (!root.supported) return pack_flags(root);
    return pack_flags(nt::numeric::divide_finite(left,root.bits));
}
