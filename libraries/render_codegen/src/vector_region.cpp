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
using Lanes=std::array<std::array<bool,4>,32>;
unsigned overwrite_horizon(std::span<const vu::Pair> body,unsigned unit) {
    Lanes written{};
    for (const auto& pair:body) {
        if (pair.end || pair.interlock || pair.debug || pair.trap || pair.immediate)
            throw std::invalid_argument("Vector region contains an end, interlock, debug, trap or literal effect");
        const auto upper=vu::decode_upper(pair.upper);
        const auto lower=vu::decode_lower(pair,unit);
        switch (upper.operation) {
        case A::nop: case A::absolute: case A::minimum: case A::maximum:
        case A::integer_to_float: case A::float_to_integer: break;
        default: throw std::invalid_argument("Vector region live-out flags or arithmetic are not yet proved");
        }
        if (upper.destination==vu::Destination::accumulator)
            throw std::invalid_argument("Vector region accumulator live-out is not proved");
        switch (lower.operation) {
        case L::load_quad: case L::store_quad: case L::move: case L::rotate: case L::branch: break;
        case L::integer_add_immediate: case L::integer_subtract_immediate:
            if (lower.result.index!=lower.left.index)
                throw std::invalid_argument("Vector region requires self-updating integer recurrences");
            break;
        case L::integer_add: case L::integer_subtract: case L::integer_and: case L::integer_or:
            if (lower.result.index) throw std::invalid_argument("Vector region integer operation is not proved");
            break; // includes the architectural lower NOP
        default: throw std::invalid_argument("Vector region lower side effect is not proved");
        }
        for (unsigned lane=0;lane<4;++lane) {
            const bool up=upper.destination==vu::Destination::vector && upper.operation!=A::nop &&
                upper.result && (upper.mask&(8U>>lane));
            const bool low=lower.result.bank==vu::Bank::vector && lower.result.index && (lower.mask&(8U>>lane));
            if (up && low && upper.result==lower.result.index)
                throw std::invalid_argument("Vector region has simultaneous writes to one vector lane");
            if (up) written[upper.result][lane]=true;
            if (low) written[lower.result.index][lane]=true;
        }
    }
    // Only changed lanes start tainted. Preserved lanes remain valid loop inputs.
    auto tainted=written;
    for (unsigned iteration=1;iteration<=8;++iteration) {
        for (const auto& pair:body) {
            const auto upper=vu::decode_upper(pair.upper);
            const auto lower=vu::decode_lower(pair,unit);
            auto next=tainted; // both units sample pre-pair values
            for (unsigned lane=0;lane<4;++lane) {
                if (upper.destination==vu::Destination::vector && upper.operation!=A::nop && upper.result && (upper.mask&(8U>>lane))) {
                    bool value=tainted[upper.left][lane];
                    if ((upper.operation==A::minimum || upper.operation==A::maximum) && upper.operand!=vu::Operand::immediate) {
                        const auto component=upper.operand==vu::Operand::vector ? lane :
                            static_cast<unsigned>(upper.operand)-static_cast<unsigned>(vu::Operand::x);
                        if (component>=4) throw std::invalid_argument("Vector region scalar operand is not proved");
                        value=value || tainted[upper.right][component];
                    }
                    next[upper.result][lane]=value;
                }
                if (lower.result.bank==vu::Bank::vector && lower.result.index && (lower.mask&(8U>>lane))) {
                    next[lower.result.index][lane]=lower.operation==L::load_quad ? false :
                        tainted[lower.left.index][lower.operation==L::rotate ? (lane+1)%4:lane];
                }
            }
            tainted=next;
        }
        bool any=false;
        for (unsigned reg=1;reg<32;++reg) for (unsigned lane=0;lane<4;++lane) any|=tainted[reg][lane];
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
    plan.drain=overwrite_horizon(body,unit);
    plan.range=analyze_element_range(body,unit);
    const auto structure=analyze_loop_structure(body,unit);
    plan.loop=*recover_pipelined_loop(body,unit,structure).loop;
    plan.startup=plan.loop.depth;
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
    static_assert(offsetof(VectorRegion,strides)==32 && offsetof(VectorRegion,output)==96);
    static_assert(sizeof(RegionStore)==16 && offsetof(VectorRegion,iteration)==1120 && sizeof(VectorRegion)==1144);
    m<<"declare i32 @"<<prefix<<"_iteration(ptr)\n"<<"declare void @"<<prefix<<"_elements(ptr, ptr, ptr)\n";
    const std::string type="{ [8 x i32], [16 x i32], [64 x { i32, i32, i32, i32 }], ptr, ptr, ptr }";
    m<<"@"<<prefix<<" = constant "<<type<<" { [8 x i32] [";
    const unsigned fields[]{plan.unit,plan.head,plan.exit,plan.startup,plan.drain,plan.range.counter,plan.range.step,unsigned(plan.loop.stores.size())};
    for (unsigned i=0;i<8;++i) m<<(i ? ", ":"")<<"i32 "<<fields[i];
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
