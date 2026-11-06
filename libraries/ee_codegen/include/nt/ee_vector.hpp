#pragma once
#include <cstdint>
#include <array>

namespace nt::ee {
// Storage shared by ahead-of-time vector code and EE transfers. Owners must
// finish active work before allowing access; this is a cooperative, single-thread
// ownership boundary, not synchronization for concurrent host access.
struct VectorStorage {
    std::array<std::array<std::uint32_t,4>,31> values{};
    bool active=false;
    std::uint32_t mac=0,status=0;
    std::array<std::uint16_t,15> integers{};
    std::uint16_t launch_address=0;
    std::array<std::uint32_t,4> accumulator{};
    std::uint32_t quotient=0,clip=0,random=0x3f800000,immediate=0;
    std::uint32_t program_counter=0; // Pair after the last completed program.
};
enum class VectorStatus : std::uint32_t { ready, pending, unsupported };
struct VectorReadResult { VectorStatus status; std::uint32_t value=0; };
// Native owners synchronize completed work before returning ready. A pending or
// unsupported read must be retryable and must not consume an event or reset state.
// The callback receives no instruction words and never decodes guest code.
struct VectorControl {
    void* context=nullptr;
    VectorReadResult (*read)(void*,unsigned selector,bool wait)=nullptr;
    // Pending/unsupported writes commit nothing. Ready means the native owner
    // applied the requested operation, including its required synchronization.
    VectorStatus (*write)(void*,unsigned selector,bool wait,std::uint32_t value)=nullptr;
};
}
