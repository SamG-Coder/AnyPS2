#include <nt/finite_arithmetic.hpp>

namespace {
uint64_t pack(nt::numeric::FiniteFloatResult value) {
    return value.bits | (uint64_t(value.underflow)<<32) |
        (uint64_t(!value.supported)<<33) | (uint64_t(value.overflow)<<35);
}
}

// Pure functions shared with the CPU arithmetic implementation. This file is
// compiled to LLVM IR during the build, then linked into AOT shader modules.
extern "C" uint64_t nt_gpu_add(uint32_t left,uint32_t right,uint32_t subtract) {
    return pack(nt::numeric::add_subtract_finite(left,right,subtract!=0,true));
}
extern "C" uint64_t nt_gpu_multiply(uint32_t left,uint32_t right) {
    return pack(nt::numeric::multiply_finite(left,right,true));
}
