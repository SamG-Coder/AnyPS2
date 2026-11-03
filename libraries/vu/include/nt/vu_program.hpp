#pragma once
#include <nt/binary.hpp>
#include <optional>
#include <vector>
#include <set>

namespace nt::vu {
// Conversion-time instruction framing, not an execution model. Unit addresses
// are instruction-pair indices. I-bit lower words are data, never lower opcodes.
struct Operation {
    std::uint32_t encoding;
    unsigned primary,function,subfunction,source,target,destination,mask;
};
struct Pair {
    unsigned address;
    Operation upper;
    std::optional<Operation> lower;
    std::optional<std::uint32_t> immediate;
    bool end,interlock,debug,trap;
};
std::vector<Pair> frame(ByteView bytes,unsigned unit,unsigned destination);
enum class Branch { none, jump, call, register_jump, register_call, equal, not_equal,
                    less_zero, greater_zero, less_equal_zero, greater_equal_zero };
struct Control {
    Branch branch=Branch::none;
    unsigned source=0,target_register=0,link_register=0;
    std::optional<unsigned> target;
};
Control control(const Pair& pair,unsigned unit);
bool direct_delayed_branch(const Pair& first,const Pair& second,unsigned unit);
struct Transfer {
    unsigned address,delay_slot;
    Control control;
    bool terminates=false;
};
struct Flow {
    std::set<unsigned> reachable,missing,indirect,complex_delay,delayed_continuations;
    std::vector<Transfer> transfers;
};
// Discover compilation candidates from explicit entry points. Link continuations
// are candidates, not proof that an indirect return will target them.
Flow discover(std::span<const Pair> pairs,unsigned unit,std::span<const unsigned> entries);
}
