#include <nt/finite_arithmetic.hpp>
#include <nt/ee_vector_math.hpp>
#include <nt/vu_quotient.hpp>
#include <nt/vu_extended.hpp>

namespace {
uint64_t pack(nt::numeric::FiniteFloatResult value) {
    return value.bits | (uint64_t(value.underflow)<<32) |
        (uint64_t(!value.supported)<<33) | (uint64_t(value.overflow)<<35);
}
// Element operations return the VU result bits; bit 63 marks an operand the VU
// would stop on, so a kernel can report the element instead of inventing a value.
// Bits 32 (underflow), 35 (overflow) and 36-39 (MADD product Z/S/U/O) carry the
// flags a recovered iteration contributes to the status sticky bits.
constexpr uint64_t unsupported=1ULL<<63;
uint64_t element(nt::numeric::FiniteFloatResult value) {
    return value.bits|(uint64_t(value.underflow)<<32)|(uint64_t(value.overflow)<<35)|
        (uint64_t(value.product_flags&15U)<<36)|(value.supported ? 0 : unsupported);
}
uint64_t element(nt::numeric::ScalarFlagResult value) { return value.bits|(value.supported ? 0 : unsupported); }
}

// Pure functions shared with the CPU arithmetic implementation. This file is
// compiled to LLVM IR during the build, then linked into AOT shader modules.
extern "C" uint64_t nt_gpu_add(uint32_t left,uint32_t right,uint32_t subtract) {
    return pack(nt::numeric::add_subtract_finite(left,right,subtract!=0,true));
}
extern "C" uint64_t nt_gpu_multiply(uint32_t left,uint32_t right) {
    return pack(nt::numeric::multiply_finite(left,right,true));
}
// Recovered VU element operations, using the same definitions as the CPU path.
extern "C" uint64_t nt_gpu_element_add(uint32_t left,uint32_t right,uint32_t subtract) {
    return element(nt::numeric::add_subtract_finite(left,right,subtract!=0,true));
}
extern "C" uint64_t nt_gpu_element_multiply(uint32_t left,uint32_t right) {
    return element(nt::numeric::multiply_finite(left,right,true));
}
extern "C" uint64_t nt_gpu_element_multiply_accumulate(uint32_t accumulator,uint32_t left,uint32_t right,uint32_t subtract) {
    return element(nt::numeric::vector_multiply_accumulate(accumulator,left,right,subtract!=0));
}
extern "C" uint64_t nt_gpu_element_min_max(uint32_t left,uint32_t right,uint32_t maximum) {
    return nt::numeric::min_max_scalar(left,right,maximum!=0);
}
extern "C" uint64_t nt_gpu_element_integer_to_float(uint32_t value,uint32_t scale) {
    return nt::numeric::integer_to_scaled_scalar(value,scale);
}
extern "C" uint64_t nt_gpu_element_float_to_integer(uint32_t value,uint32_t scale) {
    return nt::numeric::scaled_scalar_to_integer(value,scale);
}
extern "C" uint64_t nt_gpu_element_divide(uint32_t left,uint32_t right) {
    return element(nt::numeric::vu_divide_finite(left,right));
}
extern "C" uint64_t nt_gpu_element_square_root(uint32_t right) {
    return element(nt::numeric::vu_square_root_finite(right));
}
extern "C" uint64_t nt_gpu_element_reciprocal_square_root(uint32_t left,uint32_t right) {
    return element(nt::numeric::vu_reciprocal_square_root_finite(left,right));
}
extern "C" uint64_t nt_gpu_element_reciprocal_length(uint32_t x,uint32_t y,uint32_t z) {
    return element(nt::numeric::reciprocal_length_finite(x,y,z));
}
extern "C" uint64_t nt_gpu_element_sine(uint32_t x) {
    return element(nt::numeric::sine_finite(x));
}
// Matches nt_vu_efu_square_root_finite: the EFU takes the magnitude, and any
// square-root flag is an unsupported VU result.
extern "C" uint64_t nt_gpu_element_efu_square_root(uint32_t x) {
    const auto root=nt::numeric::square_root_finite(x&0x7fffffffU);
    return root.bits|(root.supported && !root.flags ? 0 : unsupported);
}
