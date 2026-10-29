#pragma once
#include <nt/ee_state.hpp>
#include <nt/ee_address.hpp>
#include <nt/ee_vector.hpp>

namespace nt::ee {
enum class Stop : std::uint32_t { budget, syscall, unmapped_code, invalid_state, memory_fault, code_write, unsupported_numeric, interrupt_pending, vector_pending, unsupported_vector, unsupported_system, breakpoint, arithmetic_overflow, trap, unpublished_code };
// Disabled performance-counter metadata. Enabling measurement requires a native
// metric provider; no synthetic cycles are accumulated here.
struct PerformanceState { std::uint32_t control=0; std::uint32_t counters[2]{}; };
// Native service accesses return Stop::budget on completion; other stops must
// preserve service state and remain retryable. No callback receives guest code.
struct IoRead { Stop status; std::uint32_t value=0; };
struct NativeIo {
    void* context=nullptr;
    IoRead (*read32)(void*,std::uint32_t)=nullptr;
    Stop (*write32)(void*,std::uint32_t,std::uint32_t)=nullptr;
    void (*poll)(void*)=nullptr;
};
// Compile-time selected address-space requests; no instruction words reach a provider.
// A provider must preserve its state on failure and implement mappings consistently
// with every compiled memory/control-flow access before it can be installed.
enum class AddressSpaceOperation : std::uint32_t { read_parameter, write_parameter, read_mapping, install_indexed, install_replacement, probe };
struct NativeAddressSpace {
    void* context=nullptr;
    IoRead (*request)(void*,AddressSpaceOperation,std::uint32_t,std::uint32_t)=nullptr;
};
// RAM is caller-owned; the generated code never fetches instructions from it.
class NativeInterrupts;
class NativeCompletions;
class NativeSifLink;
class NativeSifOutbound;
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
    NativeIo io;
    NativeInterrupts* interrupts=nullptr;
    NativeAddressSpace address_space;
    NativeCompletions* completions=nullptr;
    NativeSifLink* sif_link=nullptr;
    NativeSifOutbound* sif_outbound=nullptr;
};
// Syscall boundaries retain PC and do not retire until a native service handles them.
using Entry = Stop (*)(runtime::State*,Memory*,std::uint64_t);
}
