#pragma once
#include <nt/vu_program.hpp>
#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nt::render {
// Conversion-time structure of one VU loop iteration, recovered by symbolic
// execution of the body including its branch delay slot. This is analysis
// evidence for lifting, not a proof of installability: entry state, live-out
// registers and flags, and external entries still need separate proofs.

// An integer value as an entry integer register plus a constant, an absolute
// constant (register -1), or unknown (data dependent or non-affine).
struct AffineInteger {
    int base=-1;
    std::int32_t offset=0;
    bool known=true;
    bool operator==(const AffineInteger&) const = default;
};
enum class AccessKind { load_vector, store_vector, load_integer, store_integer };
struct MemoryAccess {
    AccessKind kind=AccessKind::load_vector;
    unsigned pair=0,reg=0,lanes=15;      // reg: vector or integer register transferred
    AffineInteger address;               // quadword address at the access
    std::int32_t stride=0;               // address change per iteration; meaningful if address.known
};
enum class CarriedValue {
    none,
    prefetched,  // read before written; the iteration's last definition is a load
    computed,    // read before written; the last definition is arithmetic or a move
};
struct LoopStructure {
    unsigned unit=1,head=0,branch=0,pairs=0;
    vu::Branch exit_branch=vu::Branch::none;
    unsigned compare_left=0,compare_right=0;
    std::optional<unsigned> counter;            // induction register tested by the backedge
    std::array<std::int32_t,16> steps{};        // per-iteration change of each integer register
    std::array<bool,16> affine_update{};        // final value is entry + constant
    std::vector<MemoryAccess> accesses;
    std::array<CarriedValue,32> carried_vectors{};
    std::array<bool,32> invariant_vectors{};    // read, never written: loop-invariant inputs
    bool carried_accumulator=false,reads_immediate=false,writes_immediate=false;
    bool carried_quotient=false,uses_quotient=false,carried_p=false,uses_p=false;
    unsigned graphics_kicks=0,clip_writes=0,clip_reads=0,mac_reads=0,status_reads=0;
    unsigned internal_branches=0,unknown_addresses=0,stationary_stores=0;
    // Each entry names a property that prevents lifting iterations as
    // independent vertex work under exact semantics.
    std::vector<std::string> blockers;
};
// pairs must be the contiguous body from the loop head to the backedge's delay
// slot. Throws for malformed input; unsupported semantics become blockers.
LoopStructure analyze_loop_structure(std::span<const vu::Pair> pairs,unsigned unit);
// Every backward conditional branch in a program image with its body.
struct LoopRegion { unsigned head,branch; };
std::vector<LoopRegion> find_loop_regions(std::span<const vu::Pair> program,unsigned unit);
}
