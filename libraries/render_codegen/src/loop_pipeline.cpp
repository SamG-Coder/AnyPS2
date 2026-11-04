#include <nt/loop_pipeline.hpp>
#include <nt/vu_arithmetic.hpp>
#include <nt/vu_lower.hpp>
#include <algorithm>
#include <map>
#include <stdexcept>

namespace nt::render {
namespace {
using O=vu::LowerOperation;
using A=vu::Arithmetic;
using Kind=ElementNodeKind;
using Op=ElementOperation;
constexpr unsigned simulated_iterations=10;
constexpr std::uint32_t absolute_base=16,top_base=17,itop_base=18;
struct Failure { std::string reason; };
[[noreturn]] void fail(std::string reason) { throw Failure{std::move(reason)}; }
class Graph {
public:
    std::uint32_t make(ElementNode node) {
        const auto [found,inserted]=index_.try_emplace(node,static_cast<std::uint32_t>(nodes_.size()));
        if (inserted) nodes_.push_back(node);
        return found->second;
    }
    std::uint32_t constant(std::uint32_t bits) { return make({Kind::constant,0,0,0,0,bits}); }
    std::uint32_t operation(Op op,std::uint32_t b,std::uint32_t c=0,std::uint32_t d=0,std::uint32_t value=0) {
        return make({Kind::operation,static_cast<std::uint32_t>(op),b,c,d,value});
    }
    const ElementNode& operator[](std::uint32_t id) const { return nodes_[id]; }
    std::size_t size() const { return nodes_.size(); }
private:
    std::vector<ElementNode> nodes_;
    std::map<ElementNode,std::uint32_t> index_;
};
unsigned operand_count(Op op) {
    switch (op) {
    case Op::absolute: case Op::integer_to_float: case Op::float_to_integer: case Op::square_root:
    case Op::sine: case Op::efu_square_root: return 1;
    case Op::multiply_add: case Op::multiply_subtract: case Op::reciprocal_length: return 3;
    default: return 2;
    }
}
// Integer registers as entry register plus constant, as in loop structure analysis.
struct Integer { int base=-1; std::int32_t offset=0; bool known=true; };
Integer add(Integer a,Integer b) {
    if (!a.known || !b.known) return {-1,0,false};
    if (b.base==-1) return {a.base,a.offset+b.offset,true};
    if (a.base==-1) return {b.base,a.offset+b.offset,true};
    return {-1,0,false};
}
Integer subtract(Integer a,Integer b) {
    if (!a.known || !b.known) return {-1,0,false};
    if (b.base==-1) return {a.base,a.offset-b.offset,true};
    if (a.base==b.base) return {-1,a.offset-b.offset,true};
    return {-1,0,false};
}
// A scalar unit result that becomes visible after a fixed number of pairs, as
// the production code generator models Q and P publication.
struct Pending { bool valid=false; std::uint32_t node=0; unsigned delay=0; };
class Simulator {
public:
    Simulator(Graph& graph,const LoopStructure& structure,unsigned unit):graph_(graph),unit_(unit) {
        const auto entry_or_invariant=[&](bool carried,std::uint32_t file,std::uint32_t index,std::uint32_t lane) {
            return graph_.make({carried ? Kind::entry : Kind::invariant,file,index,lane,0,0});
        };
        for (unsigned lane=0;lane<4;++lane) {
            vectors_[0][lane]=graph_.constant(lane==3 ? 0x3f800000U : 0U);
            for (unsigned n=1;n<32;++n)
                vectors_[n][lane]=entry_or_invariant(structure.carried_vectors[n]!=CarriedValue::none,vector_file,n,lane);
            accumulator_[lane]=entry_or_invariant(structure.carried_accumulator,accumulator_file,0,lane);
        }
        immediate_=entry_or_invariant(structure.writes_immediate,immediate_file,0,0);
        quotient_=entry_or_invariant(structure.carried_quotient,quotient_file,0,0);
        p_=entry_or_invariant(structure.carried_p,p_file,0,0);
        for (int n=1;n<16;++n) integers_[n]={n,0,true};
        for (unsigned n=1;n<16;++n) strides[n]=structure.steps[n];
    }
    std::array<std::int32_t,16> strides{};
    struct StoreEvent { unsigned iteration,pair,lane,base; std::int32_t row; std::uint32_t node; };
    std::vector<StoreEvent> stores;
    struct FlagEvent { unsigned iteration,pair; std::array<std::uint32_t,4> lanes; bool accumulate; };
    std::vector<FlagEvent> flags;
    std::string flags_unrecovered;
    void iteration(std::span<const vu::Pair> pairs,unsigned iteration) {
        for (std::size_t n=0;n<pairs.size();++n) pair(pairs[n],iteration,n+2==pairs.size());
    }
private:
    struct Cell { std::uint32_t node; unsigned iteration; };
    Graph& graph_;
    unsigned unit_;
    std::array<std::array<std::uint32_t,4>,32> vectors_{};
    std::array<std::uint32_t,4> accumulator_{};
    std::uint32_t immediate_=0,quotient_=0,p_=0;
    Pending pending_quotient_,pending_p_;
    std::array<Integer,16> integers_{};
    std::map<std::tuple<std::uint32_t,std::int32_t,unsigned>,Cell> memory_;
    static void advance(Pending& pending,std::uint32_t& visible) {
        if (!pending.valid) return;
        if (pending.delay==1) { visible=pending.node; pending={}; return; }
        if (pending.delay) --pending.delay;
    }
    static void publish(Pending& pending,std::uint32_t& visible) { if (pending.valid) visible=pending.node; pending={}; }
    Integer integer(unsigned index) const { return index<16 ? (index ? integers_[index] : Integer{-1,0,true}) : Integer{-1,0,false}; }
    std::pair<std::uint32_t,std::int32_t> address(Integer value,std::int32_t offset) const {
        if (!value.known) fail("memory address is not affine");
        const auto base=value.base<0 ? absolute_base : value.base==16 ? top_base : value.base==17 ? itop_base : static_cast<std::uint32_t>(value.base);
        // VU1 data addresses wrap at 1024 quadwords; absolute rows are reduced.
        auto row=value.offset+offset;
        if (base==absolute_base) row&=unit_ ? 1023 : 255;
        return {base,row};
    }
    std::uint32_t right_operand(const vu::Upper& upper,unsigned lane) const {
        switch (upper.operand) {
        case vu::Operand::vector: return vectors_[upper.right][lane];
        case vu::Operand::immediate: return immediate_;
        case vu::Operand::quotient: return quotient_;
        case vu::Operand::none: fail("arithmetic without a right operand");
        default: return vectors_[upper.right][static_cast<unsigned>(upper.operand)-static_cast<unsigned>(vu::Operand::x)];
        }
    }
    void pair(const vu::Pair& pair,unsigned iteration,bool backedge) {
        advance(pending_quotient_,quotient_);
        advance(pending_p_,p_);
        const auto upper=vu::decode_upper(pair.upper);
        const auto lower=vu::decode_lower(pair,unit_);
        // Both units read before either writes.
        std::vector<std::tuple<unsigned,unsigned,std::uint32_t>> vector_writes;
        std::vector<std::pair<unsigned,std::uint32_t>> accumulator_writes;
        FlagEvent flag{iteration,pair.address,{no_element_node,no_element_node,no_element_node,no_element_node},false};
        const auto write=[&](unsigned lane,std::uint32_t node) {
            flag.lanes[lane]=node;
            if (upper.destination==vu::Destination::accumulator) accumulator_writes.push_back({lane,node});
            else if (upper.destination==vu::Destination::vector) vector_writes.push_back({upper.result,lane,node});
        };
        switch (upper.operation) {
        case A::nop: break;
        case A::add: case A::subtract: case A::multiply: case A::minimum: case A::maximum: {
            const auto op=upper.operation==A::add ? Op::add : upper.operation==A::subtract ? Op::subtract :
                upper.operation==A::multiply ? Op::multiply : upper.operation==A::minimum ? Op::minimum : Op::maximum;
            for (unsigned lane=0;lane<4;++lane) if (upper.mask&(8U>>lane))
                write(lane,graph_.operation(op,vectors_[upper.left][lane],right_operand(upper,lane)));
            break;
        }
        case A::multiply_add: case A::multiply_subtract:
            for (unsigned lane=0;lane<4;++lane) if (upper.mask&(8U>>lane))
                write(lane,graph_.operation(upper.operation==A::multiply_add ? Op::multiply_add : Op::multiply_subtract,
                    accumulator_[lane],vectors_[upper.left][lane],right_operand(upper,lane)));
            break;
        case A::outer_multiply: case A::outer_subtract:
            // Outer-product status reduces a MAC word that keeps the previous W flags.
            if (flags_unrecovered.empty()) flags_unrecovered="outer-product flags depend on earlier MAC state";
            for (unsigned lane=0;lane<3;++lane) {
                const auto left=vectors_[upper.left][(lane+1)%3],right=vectors_[upper.right][(lane+2)%3];
                write(lane,upper.operation==A::outer_multiply ? graph_.operation(Op::multiply,left,right) :
                    graph_.operation(Op::multiply_subtract,accumulator_[lane],left,right));
            }
            break;
        case A::absolute:
            for (unsigned lane=0;lane<4;++lane) if (upper.mask&(8U>>lane))
                write(lane,graph_.operation(Op::absolute,vectors_[upper.left][lane]));
            break;
        case A::integer_to_float: case A::float_to_integer:
            for (unsigned lane=0;lane<4;++lane) if (upper.mask&(8U>>lane))
                write(lane,graph_.operation(upper.operation==A::integer_to_float ? Op::integer_to_float : Op::float_to_integer,
                    vectors_[upper.left][lane],0,0,upper.scale));
            break;
        default: fail("upper operation "+std::string(vu::name(upper.operation))+" is not an element operation");
        }
        if (upper.operation==A::add || upper.operation==A::subtract || upper.operation==A::multiply ||
            upper.operation==A::multiply_add || upper.operation==A::multiply_subtract) {
            flag.accumulate=upper.operation==A::multiply_add || upper.operation==A::multiply_subtract;
            flags.push_back(flag);
        }
        std::vector<std::pair<unsigned,Integer>> integer_writes;
        std::optional<std::uint32_t> immediate;
        const auto component=[&](const vu::Register& reg) {
            return vectors_[reg.index][reg.component<4 ? reg.component : 0];
        };
        switch (lower.operation) {
        case O::literal: immediate=graph_.constant(lower.literal); break;
        case O::branch: if (!backedge) fail("control flow inside the iteration"); break;
        case O::integer_add: integer_writes.push_back({lower.result.index,add(integer(lower.left.index),integer(lower.right.index))}); break;
        case O::integer_subtract: integer_writes.push_back({lower.result.index,subtract(integer(lower.left.index),integer(lower.right.index))}); break;
        case O::integer_add_immediate: integer_writes.push_back({lower.result.index,add(integer(lower.left.index),{-1,lower.offset,true})}); break;
        case O::integer_subtract_immediate: integer_writes.push_back({lower.result.index,add(integer(lower.left.index),{-1,-lower.offset,true})}); break;
        case O::integer_and: case O::integer_or: {
            const auto left=integer(lower.left.index),right=integer(lower.right.index);
            const bool left_zero=left.known && left.base==-1 && !left.offset,right_zero=right.known && right.base==-1 && !right.offset;
            integer_writes.push_back({lower.result.index,lower.operation==O::integer_or ?
                (right_zero ? left : left_zero ? right : Integer{-1,0,false}) :
                (left_zero || right_zero ? Integer{-1,0,true} : Integer{-1,0,false})});
            break;
        }
        case O::move: case O::rotate:
            for (unsigned lane=0;lane<4;++lane) if (lower.mask&(8U>>lane))
                vector_writes.push_back({lower.result.index,lane,vectors_[lower.left.index][lower.operation==O::rotate ? (lane+1)%4 : lane]});
            break;
        case O::integer_to_vector: {
            const auto value=integer(lower.left.index);
            if (!value.known || value.base!=-1) fail("iteration-dependent integer moved into a vector");
            const auto bits=static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(value.offset)));
            for (unsigned lane=0;lane<4;++lane) if (lower.mask&(8U>>lane)) vector_writes.push_back({lower.result.index,lane,graph_.constant(bits)});
            break;
        }
        case O::vector_to_integer: integer_writes.push_back({lower.result.index,{-1,0,false}}); break;
        case O::divide: case O::square_root: case O::reciprocal_square_root: {
            publish(pending_quotient_,quotient_);
            const auto node=lower.operation==O::square_root ? graph_.operation(Op::square_root,component(lower.right)) :
                graph_.operation(lower.operation==O::divide ? Op::divide : Op::reciprocal_square_root,component(lower.left),component(lower.right));
            pending_quotient_={true,node,lower.operation==O::reciprocal_square_root ? 13U : 7U};
            break;
        }
        case O::wait_quotient: publish(pending_quotient_,quotient_); break;
        case O::reciprocal_length: case O::sine: case O::efu_square_root: {
            publish(pending_p_,p_);
            const auto& v=vectors_[lower.left.index];
            const auto node=lower.operation==O::reciprocal_length ? graph_.operation(Op::reciprocal_length,v[0],v[1],v[2]) :
                graph_.operation(lower.operation==O::sine ? Op::sine : Op::efu_square_root,component(lower.left));
            pending_p_={true,node,lower.operation==O::reciprocal_length ? 24U : lower.operation==O::sine ? 29U : 12U};
            break;
        }
        case O::wait_p: publish(pending_p_,p_); break;
        case O::move_p:
            for (unsigned lane=0;lane<4;++lane) if (lower.mask&(8U>>lane)) vector_writes.push_back({lower.result.index,lane,p_});
            break;
        case O::load_quad: case O::store_quad: {
            auto base=integer(lower.address_register);
            Integer location=lower.address_update==vu::AddressUpdate::decrement ? add(base,{-1,-1,true}) : add(base,{-1,lower.offset,true});
            const auto [array,row]=address(location,0);
            for (unsigned lane=0;lane<4;++lane) if (lower.mask&(8U>>lane)) {
                const auto key=std::tuple{array,row,lane};
                if (lower.operation==O::load_quad) {
                    if (!lower.result.index) continue;
                    const auto found=memory_.find(key);
                    if (found!=memory_.end() && found->second.iteration!=iteration)
                        fail("a value flows between iterations through memory");
                    vector_writes.push_back({lower.result.index,lane,found!=memory_.end() ? found->second.node :
                        graph_.make({Kind::load,array,static_cast<std::uint32_t>(row),lane,0,0})});
                } else {
                    const auto node=vectors_[lower.left.index][lane];
                    memory_[key]={node,iteration};
                    stores.push_back({iteration,pair.address,lane,array,row,node});
                }
            }
            if (lower.address_update!=vu::AddressUpdate::none && lower.address_register)
                integer_writes.push_back({lower.address_register,lower.address_update==vu::AddressUpdate::increment ? add(base,{-1,1,true}) : location});
            break;
        }
        case O::input_top: integer_writes.push_back({lower.result.index,{16,0,true}}); break;
        case O::input_itop: integer_writes.push_back({lower.result.index,{17,0,true}}); break;
        default: fail("lower operation "+std::string(vu::name(lower.operation))+" is not an element operation");
        }
        for (const auto& [reg,lane,node]:vector_writes) if (reg) vectors_[reg][lane]=node;
        for (const auto& [lane,node]:accumulator_writes) accumulator_[lane]=node;
        for (const auto& [reg,value]:integer_writes) if (reg && reg<16) integers_[reg]=value;
        if (immediate) immediate_=*immediate;
    }
};
// Rebuild a node with every load moved by `iterations` elements of its array.
class Shifter {
public:
    Shifter(Graph& graph,const std::array<std::int32_t,16>& strides,std::int32_t iterations)
        :graph_(graph),strides_(strides),iterations_(iterations) {}
    std::uint32_t operator()(std::uint32_t id) {
        if (const auto found=memo_.find(id);found!=memo_.end()) return found->second;
        auto node=graph_[id];
        if (node.kind==Kind::load && node.a<16)
            node.b=static_cast<std::uint32_t>(static_cast<std::int32_t>(node.b)+strides_[node.a]*iterations_);
        else if (node.kind==Kind::operation) {
            const auto count=operand_count(static_cast<Op>(node.a));
            node.b=(*this)(node.b);
            if (count>1) node.c=(*this)(node.c);
            if (count>2) node.d=(*this)(node.d);
        }
        const auto result=graph_.make(node);
        memo_[id]=result;
        return result;
    }
private:
    Graph& graph_;
    const std::array<std::int32_t,16>& strides_;
    std::int32_t iterations_;
    std::map<std::uint32_t,std::uint32_t> memo_;
};
bool depends_on_entry(const Graph& graph,std::uint32_t id,std::map<std::uint32_t,bool>& memo) {
    if (const auto found=memo.find(id);found!=memo.end()) return found->second;
    const auto& node=graph[id];
    bool result=node.kind==Kind::entry;
    if (node.kind==Kind::operation) {
        const auto count=operand_count(static_cast<Op>(node.a));
        result=depends_on_entry(graph,node.b,memo) || (count>1 && depends_on_entry(graph,node.c,memo)) ||
            (count>2 && depends_on_entry(graph,node.d,memo));
    }
    memo[id]=result;
    return result;
}
}
PipelineRecovery recover_pipelined_loop(std::span<const vu::Pair> pairs,unsigned unit,const LoopStructure& structure) {
    for (const auto& blocker:structure.blockers)
        if (blocker!="accumulator carried between iterations" && blocker!="computed vector carried between iterations" &&
            blocker!="Q result carried between iterations" && blocker!="P result carried between iterations")
            return {std::nullopt,blocker};
    Graph graph;
    Simulator simulator(graph,structure,unit);
    try {
        for (unsigned n=0;n<simulated_iterations;++n) simulator.iteration(pairs,n);
    } catch (const Failure& failure) { return {std::nullopt,failure.reason}; }
    // Group store events by store site; each site stores once per iteration.
    std::map<std::pair<unsigned,unsigned>,std::vector<const Simulator::StoreEvent*>> sites;
    for (const auto& event:simulator.stores) sites[{event.pair,event.lane}].push_back(&event);
    if (sites.empty()) return {std::nullopt,"loop stores no vector data"};
    unsigned depth=0;
    std::map<std::uint32_t,bool> entry_memo;
    for (const auto& [site,events]:sites) {
        if (events.size()!=simulated_iterations) return {std::nullopt,"store does not execute once per iteration"};
        // The steady state starts at the first iteration from which stores are
        // entry-independent and each equals the previous one shifted by one element.
        unsigned first=simulated_iterations-1;
        while (first>0) {
            const auto& previous=*events[first-1];
            const auto& current=*events[first];
            if (depends_on_entry(graph,previous.node,entry_memo)) break;
            Shifter shift(graph,simulator.strides,1);
            if (shift(previous.node)!=current.node || previous.base!=current.base) break;
            --first;
        }
        if (depends_on_entry(graph,events[first]->node,entry_memo)) return {std::nullopt,"no entry-independent steady state"};
        if (first+3>simulated_iterations) return {std::nullopt,"steady state not reached within the simulated iterations"};
        depth=std::max(depth,first);
    }
    // Flags reach a steady state like stores: each instruction's lanes become
    // entry-independent and equal the previous iteration's shifted by one element.
    std::map<unsigned,std::vector<const Simulator::FlagEvent*>> flag_sites;
    for (const auto& event:simulator.flags) flag_sites[event.pair].push_back(&event);
    std::string flags_unrecovered=simulator.flags_unrecovered;
    unsigned flag_depth=0;
    for (const auto& [site,events]:flag_sites) {
        if (!flags_unrecovered.empty()) break;
        if (events.size()!=simulated_iterations) { flags_unrecovered="flag instruction does not execute once per iteration"; break; }
        const auto independent=[&](const Simulator::FlagEvent& event) {
            return std::none_of(event.lanes.begin(),event.lanes.end(),[&](std::uint32_t node) {
                return node!=no_element_node && depends_on_entry(graph,node,entry_memo);
            });
        };
        unsigned first=simulated_iterations-1;
        while (first>0) {
            const auto& previous=*events[first-1];
            const auto& current=*events[first];
            if (!independent(previous)) break;
            Shifter shift(graph,simulator.strides,1);
            bool same=true;
            for (unsigned lane=0;lane<4;++lane)
                same&=(previous.lanes[lane]==no_element_node) ? current.lanes[lane]==no_element_node :
                    shift(previous.lanes[lane])==current.lanes[lane];
            if (!same) break;
            --first;
        }
        if (!independent(*events[first]) || first+3>simulated_iterations) {
            flags_unrecovered="flag-producing instruction has no entry-independent steady state"; break;
        }
        flag_depth=std::max(flag_depth,first);
    }
    if (flags_unrecovered.empty()) depth=std::max(depth,flag_depth);
    // Express each store of iteration `depth` relative to element `depth`.
    PipelinedLoop result;
    result.depth=depth; result.strides=simulator.strides;
    result.flag_instructions=!flag_sites.empty();
    std::map<std::uint32_t,std::uint32_t> remap;
    Shifter back(graph,simulator.strides,-static_cast<std::int32_t>(depth));
    const auto emit=[&](auto&& self,std::uint32_t id) -> std::uint32_t {
        if (const auto found=remap.find(id);found!=remap.end()) return found->second;
        auto node=graph[id];
        if (node.kind==Kind::operation) {
            const auto count=operand_count(static_cast<Op>(node.a));
            node.b=self(self,node.b);
            if (count>1) node.c=self(self,node.c);
            if (count>2) node.d=self(self,node.d);
        }
        const auto index=static_cast<std::uint32_t>(result.nodes.size());
        result.nodes.push_back(node); remap[id]=index;
        return index;
    };
    for (const auto& [site,events]:sites) {
        const auto& event=*events[depth];
        const auto stride=event.base<16 ? simulator.strides[event.base] : 0;
        if (!stride) return {std::nullopt,"output row does not advance per element"};
        result.stores.push_back({event.pair,event.lane,event.base,event.row-stride*static_cast<std::int32_t>(depth),stride,
            emit(emit,back(event.node))});
    }
    if (flags_unrecovered.empty()) {
        for (const auto& [site,events]:flag_sites) {
            const auto& event=*events[depth];
            ElementFlags flags{event.pair,{no_element_node,no_element_node,no_element_node,no_element_node},event.accumulate};
            for (unsigned lane=0;lane<4;++lane)
                if (event.lanes[lane]!=no_element_node) flags.lanes[lane]=emit(emit,back(event.lanes[lane]));
            result.flags.push_back(flags);
        }
    } else result.flags_unrecovered=flags_unrecovered;
    return {std::move(result),{}};
}
}
