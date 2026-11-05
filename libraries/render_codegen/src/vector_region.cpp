#include <nt/vector_region_codegen.hpp>
#include <nt/vector_region.hpp>
#include <nt/vu_codegen.hpp>
#include <nt/vu_arithmetic.hpp>
#include <nt/vu_lower.hpp>
#include <algorithm>
#include <cstddef>
#include <sstream>
#include <stdexcept>

namespace nt::render {
namespace {
using L=vu::LowerOperation;
using A=vu::Arithmetic;
// VF0-VF31, the accumulator as register 32, then Q and P (lane 0 only).
constexpr unsigned accumulator_lanes=32,quotient_register=33,p_register=34;
using Lanes=std::array<std::array<bool,4>,35>;
bool flag_arithmetic(A operation) {
    return operation==A::add || operation==A::subtract || operation==A::multiply ||
        operation==A::multiply_add || operation==A::multiply_subtract;
}
unsigned upper_destination(const vu::Upper& upper) {
    return upper.destination==vu::Destination::accumulator ? accumulator_lanes : upper.result;
}
bool writes_upper(const vu::Upper& upper,unsigned lane) {
    return upper.operation!=A::nop && (upper.mask&(8U>>lane)) &&
        (upper.destination==vu::Destination::accumulator || (upper.destination==vu::Destination::vector && upper.result));
}
// Lanes of the right operand read for one result lane, or none for I/Q operands.
std::optional<std::pair<unsigned,unsigned>> right_lane(const vu::Upper& upper,unsigned lane) {
    if (upper.operand==vu::Operand::immediate || upper.operand==vu::Operand::quotient || upper.operand==vu::Operand::none) return std::nullopt;
    const auto component=upper.operand==vu::Operand::vector ? lane :
        static_cast<unsigned>(upper.operand)-static_cast<unsigned>(vu::Operand::x);
    if (component>=4) throw std::invalid_argument("Vector region scalar operand is not proved");
    return std::pair{upper.right,component};
}
// Returns the number of final iterations whose original replay reconstructs
// every changed VF/ACC lane and, with flag arithmetic, the MAC/status queue.
unsigned overwrite_horizon(std::span<const vu::Pair> body,unsigned unit,bool& flags) {
    Lanes written{};
    flags=false;
    for (const auto& pair:body) {
        if (pair.end || pair.interlock || pair.debug || pair.trap || pair.immediate)
            throw std::invalid_argument("Vector region contains an end, interlock, debug, trap or literal effect");
        const auto upper=vu::decode_upper(pair.upper);
        const auto lower=vu::decode_lower(pair,unit);
        switch (upper.operation) {
        case A::nop: case A::absolute: case A::minimum: case A::maximum:
        case A::integer_to_float: case A::float_to_integer: break;
        case A::add: case A::subtract: case A::multiply: case A::multiply_add: case A::multiply_subtract: flags=true; break;
        default: throw std::invalid_argument("Vector region upper operation "+std::string(vu::name(upper.operation))+" is not proved");
        }
        if (upper.destination==vu::Destination::accumulator && !flag_arithmetic(upper.operation))
            throw std::invalid_argument("Vector region accumulator write is not proved");
        switch (lower.operation) {
        case L::load_quad: case L::store_quad: case L::move: case L::rotate: case L::branch: break;
        case L::integer_add_immediate: case L::integer_subtract_immediate:
            if (lower.result.index!=lower.left.index)
                throw std::invalid_argument("Vector region requires self-updating integer recurrences");
            break;
        case L::integer_add: case L::integer_subtract: case L::integer_and: case L::integer_or:
            if (lower.result.index) throw std::invalid_argument("Vector region integer operation is not proved");
            break; // includes the architectural lower NOP
        // Q and P results: the element graph evaluates them, and the live-out
        // horizon below proves that the replay reconstructs both pipelines.
        case L::divide: case L::square_root: case L::reciprocal_square_root: case L::wait_quotient:
        case L::reciprocal_length: case L::sine: case L::efu_square_root: case L::wait_p: case L::move_p: break;
        default: throw std::invalid_argument("Vector region lower side effect is not proved");
        }
        for (unsigned lane=0;lane<4;++lane) {
            const bool up=writes_upper(upper,lane);
            const bool low=lower.result.bank==vu::Bank::vector && lower.result.index && (lower.mask&(8U>>lane));
            if (up && low && upper_destination(upper)==lower.result.index)
                throw std::invalid_argument("Vector region has simultaneous writes to one vector lane");
            if (up) written[upper_destination(upper)][lane]=true;
            if (low) written[lower.result.index][lane]=true;
        }
    }
    // Only changed lanes start tainted. Preserved lanes remain valid loop inputs.
    auto tainted=written;
    // Q and P results become visible after their issue latency, as in the
    // pipeline recovery. Only a unit the body issues can be changed by skipped
    // iterations; a result of it in flight at replay start is unknown and is
    // published, tainted, no later than the next issue.
    struct Pending { bool valid=false,taint=false; unsigned delay=0; };
    bool issues_quotient=false,issues_p=false;
    for (const auto& pair:body) {
        const auto operation=vu::decode_lower(pair,unit).operation;
        issues_quotient|=operation==L::divide || operation==L::square_root || operation==L::reciprocal_square_root;
        issues_p|=operation==L::reciprocal_length || operation==L::sine || operation==L::efu_square_root;
    }
    Pending pending_quotient{issues_quotient,issues_quotient,13},pending_p{issues_p,issues_p,29};
    tainted[quotient_register][0]=issues_quotient; tainted[p_register][0]=issues_p;
    const auto advance=[&](Pending& pending,unsigned reg) {
        if (!pending.valid) return;
        if (pending.delay==1) { tainted[reg][0]=pending.taint; pending={}; return; }
        if (pending.delay) --pending.delay;
    };
    const auto publish=[&](Pending& pending,unsigned reg) {
        if (pending.valid) tainted[reg][0]=pending.taint;
        pending={};
    };
    const auto component=[&](const vu::Register& operand) {
        if (operand.component<4) return bool(tainted[operand.index][operand.component]);
        const auto& lanes=tainted[operand.index];
        return lanes[0] || lanes[1] || lanes[2] || lanes[3];
    };
    for (unsigned iteration=1;iteration<=8;++iteration) {
        for (const auto& pair:body) {
            advance(pending_quotient,quotient_register);
            advance(pending_p,p_register);
            const auto upper=vu::decode_upper(pair.upper);
            const auto lower=vu::decode_lower(pair,unit);
            // Producers and waits publish the earlier result before this pair reads.
            if (lower.operation==L::divide || lower.operation==L::square_root || lower.operation==L::reciprocal_square_root ||
                lower.operation==L::wait_quotient) publish(pending_quotient,quotient_register);
            if (lower.operation==L::reciprocal_length || lower.operation==L::sine || lower.operation==L::efu_square_root ||
                lower.operation==L::wait_p) publish(pending_p,p_register);
            auto next=tainted; // both units sample pre-pair values
            for (unsigned lane=0;lane<4;++lane) {
                if (writes_upper(upper,lane)) {
                    bool value=tainted[upper.left][lane];
                    if (upper.operation==A::minimum || upper.operation==A::maximum || flag_arithmetic(upper.operation)) {
                        if (const auto right=right_lane(upper,lane)) value=value || tainted[right->first][right->second];
                        if (upper.operand==vu::Operand::quotient) value=value || tainted[quotient_register][0];
                    }
                    if (upper.operation==A::multiply_add || upper.operation==A::multiply_subtract)
                        value=value || tainted[accumulator_lanes][lane];
                    next[upper_destination(upper)][lane]=value;
                }
                if (lower.result.bank==vu::Bank::vector && lower.result.index && (lower.mask&(8U>>lane))) {
                    next[lower.result.index][lane]=lower.operation==L::load_quad ? false :
                        lower.operation==L::move_p ? tainted[p_register][0] :
                        tainted[lower.left.index][lower.operation==L::rotate ? (lane+1)%4:lane];
                }
            }
            switch (lower.operation) {
            case L::divide: case L::square_root: case L::reciprocal_square_root: {
                const bool taint=component(lower.right) || (lower.operation!=L::square_root && component(lower.left));
                publish(pending_quotient,quotient_register); next[quotient_register][0]=tainted[quotient_register][0];
                pending_quotient={true,taint,lower.operation==L::reciprocal_square_root ? 13U : 7U};
                break;
            }
            case L::wait_quotient: publish(pending_quotient,quotient_register); next[quotient_register][0]=tainted[quotient_register][0]; break;
            case L::reciprocal_length: case L::sine: case L::efu_square_root: {
                const bool taint=lower.operation==L::reciprocal_length ?
                    tainted[lower.left.index][0] || tainted[lower.left.index][1] || tainted[lower.left.index][2] : component(lower.left);
                publish(pending_p,p_register); next[p_register][0]=tainted[p_register][0];
                pending_p={true,taint,lower.operation==L::reciprocal_length ? 24U : lower.operation==L::sine ? 29U : 12U};
                break;
            }
            case L::wait_p: publish(pending_p,p_register); next[p_register][0]=tainted[p_register][0]; break;
            default: break;
            }
            tainted=next;
        }
        bool any=(pending_quotient.valid && pending_quotient.taint) || (pending_p.valid && pending_p.taint);
        for (unsigned reg=1;reg<tainted.size();++reg) for (unsigned lane=0;lane<4;++lane) any|=tainted[reg][lane];
        if (!any) return iteration;
    }
    throw std::invalid_argument("Vector region live-out lanes depend on skipped iterations");
}
bool valid_symbol(std::string_view symbol) {
    const auto letter=[](char c){return (c>='a' && c<='z') || (c>='A' && c<='Z') || c=='_';};
    return !symbol.empty() && letter(symbol.front()) && std::all_of(symbol.begin(),symbol.end(),[&](char c){return letter(c) || (c>='0' && c<='9');});
}
}
VectorRegionPlan analyze_vector_region(std::span<const vu::Pair> program,unsigned unit,std::span<const unsigned> entries,unsigned head) {
    if (unit>1 || program.empty() || entries.empty()) throw std::invalid_argument("Vector region requires a complete image and explicit entries");
    for (std::size_t i=1;i<program.size();++i)
        if (program[i].address!=program[i-1].address+1) throw std::invalid_argument("Vector region image is not contiguous");
    const auto regions=find_loop_regions(program,unit);
    std::optional<LoopRegion> selected;
    for (const auto& region:regions) if (region.head==head) {
        if (selected) throw std::invalid_argument("Vector region has ambiguous backedges");
        selected=region;
    }
    if (!selected || selected->branch+1>program.back().address) throw std::invalid_argument("Vector region has no complete loop");
    const unsigned exit=selected->branch+2;
    if (exit>program.back().address || exit>=(unit ? 2048U:512U))
        throw std::invalid_argument("Vector region requires a declared non-wrapping exit continuation");
    for (const auto entry:entries)
        if (entry<program.front().address || entry>program.back().address) throw std::invalid_argument("Vector region entry is outside its image");
    // Other paths into the loop body need no proof. The service replaces only
    // execution that reaches the head as an ordinary pair, after validating the
    // complete architectural state there, and reproduces running the loop from
    // the head to its exit. Execution entering the body elsewhere (exports,
    // direct or register jumps, launch-supplied ITOP targets) runs the original
    // compiled blocks until it reaches the head. A head that executes as a
    // branch delay slot is never an ordinary entry, so it stays rejected.
    const auto before=std::find_if(program.begin(),program.end(),[&](const vu::Pair& pair){return pair.address+1==head;});
    if (before!=program.end() && (vu::control(*before,unit).branch!=vu::Branch::none || before->end))
        throw std::invalid_argument("Vector region entry is an external delay slot");
    VectorRegionPlan plan;
    plan.unit=unit; plan.head=head; plan.exit=exit;
    const auto body=program.subspan(head-program.front().address,exit-head);
    plan.body.assign(body.begin(),body.end());
    bool flags=false;
    plan.drain=overwrite_horizon(body,unit,flags);
    // Flags of the first replayed iterations come from not yet reconstructed
    // registers. With flag arithmetic, those iterations only warm up registers;
    // two further iterations then run on exact values and republish the MAC queue
    // (four results, visible four pairs later) from exact instructions.
    if (flags) {
        if (body.size()<4) throw std::invalid_argument("Vector region flag replay needs at least four pairs per iteration");
        plan.flag_replay=2; plan.drain+=plan.flag_replay;
    }
    for (const auto& pair:body) {
        const auto operation=vu::decode_lower(pair,unit).operation;
        if (operation==L::divide || operation==L::square_root || operation==L::reciprocal_square_root) plan.pipelines|=1U;
        if (operation==L::reciprocal_length || operation==L::sine || operation==L::efu_square_root) plan.pipelines|=2U;
    }
    plan.range=analyze_element_range(body,unit);
    const auto structure=analyze_loop_structure(body,unit);
    plan.loop=*recover_pipelined_loop(body,unit,structure).loop;
    plan.startup=plan.loop.depth;
    if (flags && plan.loop.flags.empty())
        throw std::invalid_argument("Vector region live-out flags are not proved: "+plan.loop.flags_unrecovered);
    if (!flags && !plan.loop.flags.empty()) throw std::logic_error("Vector region flag analyses disagree");
    if (plan.drain>8) throw std::invalid_argument("Vector region replay exceeds eight iterations");
    // Entry validation covers all original iterations, including short loops.
    // Shifted recovered loads are only used at or after the proven startup.
    plan.range.minimum_element=0;
    std::erase_if(plan.range.reads,[](const auto& read){return read.selected;});
    if (plan.loop.stores.size()>64 || plan.startup>8) throw std::invalid_argument("Vector region exceeds bounded output or startup capacity");
    return plan;
}
void emit_vector_region_llvm(std::ostream& boundary,std::ostream& elements,std::ostream& binding,
    const VectorRegionPlan& plan,std::string_view symbol) {
    if (!valid_symbol(symbol)) throw std::invalid_argument("Invalid vector region symbol");
    // Independent modules keep each code generator's declarations authoritative.
    std::ostringstream b,e,m;
    const auto prefix=std::string(symbol);
    vu::emit_llvm(b,plan.body,plan.unit,prefix+"_iteration");
    emit_element_kernel_llvm(e,plan.loop,plan.unit,false,prefix+"_elements");
    emit_element_range_llvm(m,plan.range,prefix+"_guard");
    static_assert(offsetof(VectorRegion,strides)==44 && offsetof(VectorRegion,output)==108);
    static_assert(sizeof(RegionStore)==16 && offsetof(VectorRegion,iteration)==1136 && sizeof(VectorRegion)==1160);
    m<<"declare i32 @"<<prefix<<"_iteration(ptr)\n"<<"declare void @"<<prefix<<"_elements(ptr, ptr, ptr)\n";
    const std::string type="{ [11 x i32], [16 x i32], [64 x { i32, i32, i32, i32 }], ptr, ptr, ptr }";
    m<<"@"<<prefix<<" = constant "<<type<<" { [11 x i32] [";
    const unsigned fields[]{plan.unit,plan.head,plan.exit,plan.startup,plan.drain,plan.range.counter,plan.range.step,
        unsigned(plan.loop.stores.size()),plan.flag_replay,plan.range.trip_offset,plan.pipelines};
    for (unsigned i=0;i<11;++i) m<<(i ? ", ":"")<<"i32 "<<fields[i];
    m<<"], [16 x i32] [";
    for (unsigned i=0;i<16;++i) m<<(i ? ", ":"")<<"i32 "<<plan.range.steps[i];
    m<<"], [64 x { i32, i32, i32, i32 }] [";
    for (unsigned i=0;i<64;++i) {
        m<<(i ? ", ":"")<<"{ i32, i32, i32, i32 } ";
        if (i>=plan.loop.stores.size()) m<<"zeroinitializer";
        else { const auto& s=plan.loop.stores[i]; m<<"{ i32 "<<s.base<<", i32 "<<s.row<<", i32 "<<s.stride<<", i32 "<<s.lane<<" }"; }
    }
    m<<"], ptr @"<<prefix<<"_iteration, ptr @"<<prefix<<"_guard, ptr @"<<prefix<<"_elements }, align 8\n";
    boundary<<b.str(); elements<<e.str(); binding<<m.str();
    if (!boundary || !elements || !binding) throw std::runtime_error("Cannot emit vector region modules");
}
}
