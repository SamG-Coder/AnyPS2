#pragma once
#include <nt/ee_state.hpp>
#include <nt/ee_vector.hpp>

namespace nt::ee {
enum class Stop : std::uint32_t { budget, syscall, unmapped_code, invalid_state, memory_fault, code_write, unsupported_numeric, interrupt_pending, vector_pending, unsupported_vector, unsupported_system, breakpoint, arithmetic_overflow };
// Disabled performance-counter metadata. Enabling measurement requires a native
// metric provider; no synthetic cycles are accumulated here.
struct PerformanceState { std::uint32_t control=0; std::uint32_t counters[2]{}; };
// RAM is caller-owned; the generated code never fetches instructions from it.
struct Memory {
    std::uint8_t* data=nullptr;
    std::uint64_t size=0;
    std::uint32_t fault_address=0;
    // Cooperative native producers publish interrupt classes in bits 8..15
    // between calls. The runner never acknowledges or drops pending events.
    std::uint32_t pending_interrupts=0;
    VectorControl vector_control;
    VectorStorage* vectors=nullptr;
    PerformanceState performance;
};
// Syscall boundaries retain PC and do not retire until a native service handles them.
using Entry = Stop (*)(runtime::State*,Memory*,std::uint64_t);
}
