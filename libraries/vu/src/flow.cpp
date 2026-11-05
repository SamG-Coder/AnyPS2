#include <nt/vu_program.hpp>
#include <deque>
#include <map>
#include <stdexcept>

namespace nt::vu {
Control control(const Pair& pair,unsigned unit) {
    if (unit>1) throw std::invalid_argument("Invalid VU unit");
    Control result;
    if (!pair.lower) return result;
    const auto& lower=*pair.lower;
    switch (lower.primary) {
    case 32: result.branch=Branch::jump; break;
    case 33: result.branch=Branch::call; break;
    case 36: result.branch=Branch::register_jump; break;
    case 37: result.branch=Branch::register_call; break;
    case 40: result.branch=Branch::equal; break;
    case 41: result.branch=Branch::not_equal; break;
    case 44: result.branch=Branch::less_zero; break;
    case 45: result.branch=Branch::greater_zero; break;
    case 46: result.branch=Branch::less_equal_zero; break;
    case 47: result.branch=Branch::greater_equal_zero; break;
    default: return result;
    }
    result.source=lower.source&15U;
    result.target_register=lower.target&15U;
    if (result.branch==Branch::call || result.branch==Branch::register_call)
        result.link_register=lower.target&15U;
    if (result.branch!=Branch::register_jump && result.branch!=Branch::register_call) {
        const auto encoded=lower.encoding&0x7ffU;
        const auto displacement=static_cast<int>(encoded)-(encoded&0x400U ? 2048 : 0);
        const auto mask=unit ? 2047U : 511U;
        result.target=static_cast<unsigned>(static_cast<int>(pair.address)+1+displacement)&mask;
    }
    return result;
}
bool direct_delayed_branch(const Pair& first,const Pair& second,unsigned unit) {
    const auto a=control(first,unit),b=control(second,unit);
    return !first.end && !second.end && a.target && b.target &&
        a.branch!=Branch::call && b.branch!=Branch::call;
}
bool exclusive_delayed_branch(const Pair& first,const Pair& second,unsigned unit) {
    if (!direct_delayed_branch(first,second,unit)) return false;
    const auto a=control(first,unit),b=control(second,unit);
    if (a.source!=b.source) return false;
    const auto either=[&](Branch left,Branch right) {
        return (a.branch==left && b.branch==right) || (a.branch==right && b.branch==left);
    };
    if (either(Branch::equal,Branch::not_equal)) return a.target_register==b.target_register;
    return either(Branch::less_zero,Branch::greater_zero) || either(Branch::less_zero,Branch::greater_equal_zero) ||
        either(Branch::greater_zero,Branch::less_equal_zero);
}
Flow discover(std::span<const Pair> pairs,unsigned unit,std::span<const unsigned> entries) {
    if (unit>1 || entries.empty()) throw std::invalid_argument("VU flow needs a unit and entry points");
    const auto mask=unit ? 2047U : 511U;
    std::map<unsigned,const Pair*> code;
    for (const auto& pair:pairs)
        if (pair.address>mask || !code.emplace(pair.address,&pair).second)
            throw std::invalid_argument("Invalid or duplicate VU instruction address");
    for (const auto entry:entries) if (entry>mask) throw std::invalid_argument("Invalid VU entry point");
    std::deque<unsigned> pending(entries.begin(),entries.end());
    std::set<unsigned> visited;
    Flow result;
    const auto find=[&](unsigned address)->const Pair* {
        const auto found=code.find(address);
        if (found==code.end()) { result.missing.insert(address); return nullptr; }
        result.reachable.insert(address); return found->second;
    };
    while (!pending.empty()) {
        const auto address=pending.front(); pending.pop_front();
        if (!visited.insert(address).second) continue;
        const auto* pair=find(address);
        if (!pair) continue;
        const auto branch=control(*pair,unit);
        const auto next=(address+1)&mask;
        if (branch.branch==Branch::none && !pair->end) { pending.push_back(next); continue; }
        result.transfers.push_back({address,next,branch,pair->end});
        const auto* slot=find(next);
        if (!slot) continue;
        if (direct_delayed_branch(*pair,*slot,unit)) {
            // Both branches execute. One pair at the first selected destination
            // is the second branch's delay slot. If the second is not taken,
            // execution continues after that pair, not after the original slot.
            bool supported=true;
            // When only one can be taken, the first destination is reached as
            // ordinary code, never as the second branch's delay slot.
            const bool exclusive=exclusive_delayed_branch(*pair,*slot,unit);
            std::set<unsigned> destinations;
            if (!exclusive) destinations.insert(*branch.target);
            if (branch.branch!=Branch::jump) destinations.insert((address+2)&mask);
            if (exclusive) for (const auto destination:{*branch.target,(address+2)&mask}) {
                result.delayed_continuations.insert(destination);
                pending.push_back(destination);
            }
            for (const auto destination:destinations) {
                const auto* delay=find(destination);
                if (!delay || delay->end || control(*delay,unit).branch!=Branch::none) supported=false;
                result.delayed_continuations.insert((destination+1)&mask);
                pending.push_back((destination+1)&mask);
            }
            const auto second=control(*slot,unit);
            result.delayed_continuations.insert(*second.target);
            pending.push_back(*second.target);
            if (!supported) result.complex_delay.insert(address);
            continue;
        }
        // More deeply chained transfers and E/branch combinations require
        // further ordering support; retain explicit diagnostics for those.
        if (slot->end || control(*slot,unit).branch!=Branch::none || (pair->end && branch.branch!=Branch::none)) {
            result.complex_delay.insert(address); continue;
        }
        if (pair->end) continue;
        if (branch.target) pending.push_back(*branch.target);
        else result.indirect.insert(address);
        if (branch.branch!=Branch::jump && branch.branch!=Branch::register_jump)
            pending.push_back((address+2)&mask);
    }
    return result;
}
}
