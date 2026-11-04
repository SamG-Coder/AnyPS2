#include <nt/loop_structure.hpp>
#include <nt/vu_arithmetic.hpp>
#include <nt/vu_lower.hpp>
#include <stdexcept>

namespace nt::render {
namespace {
using O=vu::LowerOperation;
using A=vu::Arithmetic;
// Symbolic bases for launch inputs that are invariant within a launch.
constexpr int top_base=16,itop_base=17;
bool conditional(vu::Branch branch) {
    switch (branch) {
    case vu::Branch::equal: case vu::Branch::not_equal: case vu::Branch::less_zero: case vu::Branch::greater_zero:
    case vu::Branch::less_equal_zero: case vu::Branch::greater_equal_zero: return true;
    default: return false;
    }
}
AffineInteger unknown() { return {-1,0,false}; }
AffineInteger add(AffineInteger a,AffineInteger b) {
    if (!a.known || !b.known) return unknown();
    if (b.base==-1) return {a.base,a.offset+b.offset,true};
    if (a.base==-1) return {b.base,a.offset+b.offset,true};
    return unknown();
}
AffineInteger subtract(AffineInteger a,AffineInteger b) {
    if (!a.known || !b.known) return unknown();
    if (b.base==-1) return {a.base,a.offset-b.offset,true};
    if (a.base==b.base) return {-1,a.offset-b.offset,true};
    return unknown();
}
// Per-iteration effects are applied in pair order: both units read their
// operands before either unit's results are written.
class Iteration {
public:
    explicit Iteration(LoopStructure& out):out_(out) {
        for (int n=1;n<16;++n) integers_[n]={n,0,true};
        integers_[0]={-1,0,true};
    }
    void read_vector(unsigned index) {
        if (!index || index>31) return;
        if (!written_[index]) live_in_[index]=true;
    }
    void write_vector(unsigned index,bool loaded) {
        if (!index || index>31) return;
        written_[index]=true; loaded_[index]=loaded;
    }
    void read_accumulator() { if (!accumulator_written_) accumulator_live_in_=true; }
    void write_accumulator() { accumulator_written_=true; }
    AffineInteger integer(unsigned index) const { return index<16 ? integers_[index] : unknown(); }
    void set_integer(unsigned index,AffineInteger value) {
        if (index && index<16) { pending_integers_.push_back({index,value}); out_.integer_writes.push_back(value); }
    }
    void read_quotient() { out_.uses_quotient=true; if (!quotient_written_) quotient_live_in_=true; }
    void write_quotient() { quotient_written_=true; }
    void read_p() { out_.uses_p=true; if (!p_written_) p_live_in_=true; }
    void write_p() { p_written_=true; }
    void read_clip() { ++out_.clip_reads; if (!clip_written_) clip_live_in_=true; }
    void write_clip() { ++out_.clip_writes; clip_written_=true; }
    void access(AccessKind kind,unsigned pair,unsigned reg,unsigned lanes,unsigned address_register,
                std::int32_t offset,vu::AddressUpdate update) {
        auto base=integer(address_register);
        if (update==vu::AddressUpdate::decrement) {
            base=add(base,{-1,-1,true});
            if (address_register) { integers_[address_register]=base; out_.integer_writes.push_back(base); }
        }
        out_.accesses.push_back({kind,pair,reg,lanes,add(base,{-1,offset,true}),0});
        if (update==vu::AddressUpdate::increment && address_register) {
            integers_[address_register]=add(base,{-1,1,true}); out_.integer_writes.push_back(integers_[address_register]);
        }
    }
    void commit() {
        for (const auto& [index,value]:pending_integers_) integers_[index]=value;
        pending_integers_.clear();
    }
    void finish() {
        for (unsigned n=1;n<16;++n) {
            const auto& value=integers_[n];
            out_.affine_update[n]=value.known && value.base==int(n);
            out_.steps[n]=out_.affine_update[n] ? value.offset : 0;
        }
        for (unsigned n=1;n<32;++n) {
            if (live_in_[n] && written_[n]) out_.carried_vectors[n]=loaded_[n] ? CarriedValue::prefetched : CarriedValue::computed;
            out_.invariant_vectors[n]=live_in_[n] && !written_[n];
        }
        out_.carried_accumulator=accumulator_live_in_ && accumulator_written_;
        out_.carried_quotient=quotient_live_in_ && quotient_written_;
        out_.carried_p=p_live_in_ && p_written_;
        clip_carried=clip_live_in_ && clip_written_;
    }
    bool clip_carried=false;
private:
    LoopStructure& out_;
    std::array<AffineInteger,16> integers_{};
    std::vector<std::pair<unsigned,AffineInteger>> pending_integers_;
    std::array<bool,32> live_in_{},written_{},loaded_{};
    bool accumulator_live_in_=false,accumulator_written_=false;
    bool quotient_live_in_=false,quotient_written_=false,p_live_in_=false,p_written_=false;
    bool clip_live_in_=false,clip_written_=false;
};
void upper_effects(Iteration& iteration,LoopStructure& out,const vu::Upper& upper,std::vector<std::pair<unsigned,bool>>& writes,bool& accumulator) {
    switch (upper.operation) {
    case A::nop: return;
    case A::clip:
        iteration.read_vector(upper.left); iteration.read_vector(upper.right); iteration.write_clip(); return;
    default: break;
    }
    iteration.read_vector(upper.left);
    const bool binary=upper.operation!=A::absolute && upper.operation!=A::integer_to_float && upper.operation!=A::float_to_integer;
    if (binary) {
        if (upper.operand==vu::Operand::immediate) out.reads_immediate=true;
        else if (upper.operand==vu::Operand::quotient) iteration.read_quotient();
        else iteration.read_vector(upper.right);
    }
    if (upper.operation==A::multiply_add || upper.operation==A::multiply_subtract || upper.operation==A::outer_subtract)
        iteration.read_accumulator();
    if (upper.destination==vu::Destination::accumulator) accumulator=true;
    else if (upper.destination==vu::Destination::vector) writes.push_back({upper.result,false});
}
}
LoopStructure analyze_loop_structure(std::span<const vu::Pair> pairs,unsigned unit) {
    if (unit>1 || pairs.size()<3 || pairs.size()>2048) throw std::invalid_argument("Loop structure needs 3-2048 pairs and a valid VU unit");
    for (std::size_t n=1;n<pairs.size();++n)
        if (pairs[n].address!=pairs[n-1].address+1) throw std::invalid_argument("Loop structure requires contiguous pairs");
    const auto& backedge=pairs[pairs.size()-2];
    const auto exit=vu::control(backedge,unit);
    if (!conditional(exit.branch) || !exit.target || *exit.target!=pairs.front().address)
        throw std::invalid_argument("Loop structure requires a conditional backedge to the first pair");
    LoopStructure out;
    out.unit=unit; out.head=pairs.front().address; out.branch=backedge.address;
    out.pairs=static_cast<unsigned>(pairs.size()); out.exit_branch=exit.branch;
    out.compare_left=exit.source; out.compare_right=exit.target_register;
    Iteration iteration(out);
    std::optional<AffineInteger> compared_left,compared_right;
    bool unsupported=false;
    for (std::size_t n=0;n<pairs.size();++n) {
        const auto& pair=pairs[n];
        if (pair.end) out.blockers.push_back("program end flag inside the loop");
        const auto upper=vu::decode_upper(pair.upper);
        const auto lower=vu::decode_lower(pair,unit);
        std::vector<std::pair<unsigned,bool>> vector_writes;
        bool accumulator=false;
        if (upper.operation==A::unsupported) unsupported=true;
        upper_effects(iteration,out,upper,vector_writes,accumulator);
        const auto vector_left=[&] { if (lower.left.bank==vu::Bank::vector) iteration.read_vector(lower.left.index); };
        const auto vector_right=[&] { if (lower.right.bank==vu::Bank::vector) iteration.read_vector(lower.right.index); };
        switch (lower.operation) {
        case O::literal: out.writes_immediate=true; break;
        case O::branch: {
            if (n+2==pairs.size()) {
                compared_left=iteration.integer(exit.source); compared_right=iteration.integer(exit.target_register);
            } else ++out.internal_branches;
            break;
        }
        case O::integer_add: iteration.set_integer(lower.result.index,add(iteration.integer(lower.left.index),iteration.integer(lower.right.index))); break;
        case O::integer_subtract: iteration.set_integer(lower.result.index,subtract(iteration.integer(lower.left.index),iteration.integer(lower.right.index))); break;
        case O::integer_add_immediate: iteration.set_integer(lower.result.index,add(iteration.integer(lower.left.index),{-1,lower.offset,true})); break;
        case O::integer_subtract_immediate: iteration.set_integer(lower.result.index,add(iteration.integer(lower.left.index),{-1,-lower.offset,true})); break;
        case O::integer_and: case O::integer_or: {
            const auto left=iteration.integer(lower.left.index),right=iteration.integer(lower.right.index);
            const bool left_zero=left.known && left.base==-1 && !left.offset,right_zero=right.known && right.base==-1 && !right.offset;
            const auto value=lower.operation==O::integer_or ? (right_zero ? left : left_zero ? right : unknown()) :
                (left_zero || right_zero ? AffineInteger{-1,0,true} : unknown());
            iteration.set_integer(lower.result.index,value);
            break;
        }
        case O::move: case O::rotate: vector_left(); vector_writes.push_back({lower.result.index,false}); break;
        case O::vector_to_integer: vector_left(); iteration.set_integer(lower.result.index,unknown()); break;
        case O::integer_to_vector: vector_writes.push_back({lower.result.index,false}); break;
        case O::divide: case O::square_root: case O::reciprocal_square_root:
            vector_left(); vector_right(); iteration.write_quotient(); break;
        case O::wait_quotient: break;
        case O::reciprocal_length: case O::sine: case O::efu_square_root:
            vector_left(); iteration.write_p(); break;
        case O::move_p: iteration.read_p(); vector_writes.push_back({lower.result.index,false}); break;
        case O::wait_p: break;
        case O::load_quad:
            iteration.access(AccessKind::load_vector,pair.address,lower.result.index,lower.mask,lower.address_register,lower.offset,lower.address_update);
            vector_writes.push_back({lower.result.index,true});
            break;
        case O::store_quad:
            vector_left();
            iteration.access(AccessKind::store_vector,pair.address,lower.left.index,lower.mask,lower.address_register,lower.offset,lower.address_update);
            break;
        case O::load_integer:
            iteration.access(AccessKind::load_integer,pair.address,lower.result.index,lower.mask,lower.address_register,lower.offset,lower.address_update);
            iteration.set_integer(lower.result.index,unknown());
            break;
        case O::store_integer:
            iteration.access(AccessKind::store_integer,pair.address,lower.left.index,lower.mask,lower.address_register,lower.offset,lower.address_update);
            break;
        case O::input_top: iteration.set_integer(lower.result.index,{top_base,0,true}); break;
        case O::input_itop: iteration.set_integer(lower.result.index,{itop_base,0,true}); break;
        case O::mac_and: case O::mac_or: case O::mac_equal: ++out.mac_reads; iteration.set_integer(lower.result.index,unknown()); break;
        case O::status_and: case O::status_or: case O::status_equal: ++out.status_reads; iteration.set_integer(lower.result.index,unknown()); break;
        case O::clip_and: case O::clip_or: case O::clip_equal: case O::clip_get:
            iteration.read_clip(); iteration.set_integer(lower.result.index,unknown()); break;
        case O::clip_set: iteration.write_clip(); break;
        case O::graphics_kick: ++out.graphics_kicks; break;
        case O::unsupported: unsupported=true; break;
        }
        for (const auto& [index,loaded]:vector_writes) iteration.write_vector(index,loaded);
        if (accumulator) iteration.write_accumulator();
        iteration.commit();
    }
    iteration.finish();
    out.compared_left=compared_left.value_or(unknown()); out.compared_right=compared_right.value_or(unknown());
    if (unsupported) out.blockers.push_back("unsupported VU operation");
    // The counter is the compared register that changes by a constant per iteration.
    for (const auto reg:{exit.source,exit.target_register})
        if (reg && reg<16 && out.affine_update[reg] && out.steps[reg]) { out.counter=reg; break; }
    const auto invariant=[&](std::optional<AffineInteger> value,unsigned reg) {
        return !reg || (value && value->known && (value->base==-1 || value->base>=top_base || !out.steps[value->base]));
    };
    if (!out.counter) out.blockers.push_back("backedge does not test an affine induction register");
    else if (!invariant(compared_left,exit.source==*out.counter ? 0 : exit.source) ||
             !invariant(compared_right,exit.target_register==*out.counter ? 0 : exit.target_register))
        out.blockers.push_back("backedge compares against a value that changes per iteration");
    for (auto& access:out.accesses) {
        if (!access.address.known) { ++out.unknown_addresses; continue; }
        access.stride=access.address.base>=0 && access.address.base<16 ? out.steps[access.address.base] : 0;
        if (access.address.base>=0 && access.address.base<16 && !out.affine_update[access.address.base]) ++out.unknown_addresses;
        else if (!access.stride && (access.kind==AccessKind::store_vector || access.kind==AccessKind::store_integer)) ++out.stationary_stores;
    }
    if (out.unknown_addresses) out.blockers.push_back("memory address is not affine in the induction registers");
    if (out.stationary_stores) out.blockers.push_back("store writes the same address every iteration");
    if (out.internal_branches) out.blockers.push_back("control flow inside the iteration");
    if (out.graphics_kicks) out.blockers.push_back("graphics kick inside the iteration");
    if (iteration.clip_carried) out.blockers.push_back("clip-flag history carried between iterations");
    else if (out.clip_reads) out.blockers.push_back("clip-flag test inside the iteration");
    if (out.mac_reads || out.status_reads) out.blockers.push_back("MAC or status flag test inside the iteration");
    if (out.carried_quotient) out.blockers.push_back("Q result carried between iterations");
    if (out.carried_p) out.blockers.push_back("P result carried between iterations");
    if (out.carried_accumulator) out.blockers.push_back("accumulator carried between iterations");
    for (unsigned n=1;n<32;++n) if (out.carried_vectors[n]==CarriedValue::computed) {
        out.blockers.push_back("computed vector carried between iterations"); break;
    }
    return out;
}
std::vector<LoopRegion> find_loop_regions(std::span<const vu::Pair> program,unsigned unit) {
    std::vector<LoopRegion> regions;
    for (std::size_t n=0;n+1<program.size();++n) {
        const auto transfer=vu::control(program[n],unit);
        if (!conditional(transfer.branch) || !transfer.target || *transfer.target>program[n].address ||
            *transfer.target<program.front().address) continue;
        regions.push_back({*transfer.target,program[n].address});
    }
    return regions;
}
}
