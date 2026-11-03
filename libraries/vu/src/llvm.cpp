#include <nt/vu_codegen.hpp>
#include <nt/vu_arithmetic.hpp>
#include <nt/vu_lower.hpp>
#include <nt/vu_native.hpp>
#include <cstddef>
#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace nt::vu {
namespace {
struct Store { std::string type,value; std::size_t offset; std::string address{}; };
bool upper_supported(Arithmetic operation) {
    switch (operation) {
    case Arithmetic::nop: case Arithmetic::absolute: case Arithmetic::minimum: case Arithmetic::maximum:
    case Arithmetic::integer_to_float: case Arithmetic::float_to_integer:
    case Arithmetic::add: case Arithmetic::subtract: case Arithmetic::multiply:
    case Arithmetic::multiply_add: case Arithmetic::multiply_subtract:
    case Arithmetic::outer_multiply: case Arithmetic::outer_subtract: case Arithmetic::clip: return true;
    default: return false;
    }
}
class Writer {
public:
    std::ostringstream out;
    std::vector<Store> pending;
    std::string value(const std::string& expression) {
        const auto id="%v"+std::to_string(next_++);
        out<<"  "<<id<<" = "<<expression<<'\n'; return id;
    }
    std::string pointer(std::size_t offset) { return value("getelementptr i8, ptr %state, i64 "+std::to_string(offset)); }
    std::string load(const std::string& type,std::size_t offset) {
        return value("load "+type+", ptr "+pointer(offset)+", align 1");
    }
    std::string integer(unsigned index) { return index ? load("i16",offsetof(State,integers)+(index-1)*2) : "0"; }
    std::string vector(unsigned index,unsigned lane) {
        return index ? load("i32",vector_offset(index,lane)) : lane==3 ? "1065353216" : "0";
    }
    void integer_result(unsigned index,const std::string& result) {
        if (index) pending.push_back({"i16",result,offsetof(State,integers)+(index-1)*2});
    }
    void vector_result(unsigned index,unsigned lane,const std::string& result) {
        if (index) pending.push_back({"i32",result,vector_offset(index,lane)});
    }
    void publish_quotient() {
        const auto active=value("icmp ne i32 "+load("i32",offsetof(State,quotient_pending))+", 0");
        const auto result=value("select i1 "+active+", i32 "+load("i32",offsetof(State,pending_quotient))+
            ", i32 "+load("i32",offsetof(State,quotient)));
        const auto flags=load("i32",offsetof(State,quotient_flags));
        const auto previous=load("i32",offsetof(State,status));
        const auto sticky=value("shl i32 "+flags+", 6");
        const auto merged=value("or i32 "+value("and i32 "+previous+", 4047")+", "+value("or i32 "+flags+", "+sticky));
        const auto status=value("select i1 "+active+", i32 "+merged+", i32 "+previous);
        pending.push_back({"i32",result,offsetof(State,quotient)});
        pending.push_back({"i32",status,offsetof(State,status)});
        const auto visible_previous=load("i32",offsetof(State,visible_status));
        const auto visible_merged=value("or i32 "+value("and i32 "+visible_previous+", 4047")+", "+value("or i32 "+flags+", "+sticky));
        pending.push_back({"i32",value("select i1 "+active+", i32 "+visible_merged+", i32 "+visible_previous),offsetof(State,visible_status)});
        pending.push_back({"i32","0",offsetof(State,quotient_pending)});
        pending.push_back({"i32","0",offsetof(State,quotient_read_delay)});
        commit();
    }
    void publish_p() {
        const auto active=value("icmp ne i32 "+load("i32",offsetof(State,p_pending))+", 0");
        const auto result=value("select i1 "+active+", i32 "+load("i32",offsetof(State,pending_p))+
            ", i32 "+load("i32",offsetof(State,scalar_p)));
        pending.push_back({"i32",result,offsetof(State,scalar_p)});
        pending.push_back({"i32","0",offsetof(State,p_pending)});
        commit();
    }
    void advance_visibility(std::size_t offset) {
        const auto delay=load("i32",offset);
        const auto active=value("icmp ne i32 "+delay+", 0");
        const auto remaining=value("select i1 "+active+", i32 "+value("sub i32 "+delay+", 1")+", i32 0");
        pending.push_back({"i32",remaining,offset});
        commit();
    }
    void advance_mac() {
        const auto mask=load("i32",offsetof(State,pending_mac_mask));
        const auto idle=value("icmp eq i32 "+mask+", 0");
        const auto previous=value("select i1 "+idle+", i32 "+load("i32",offsetof(State,mac))+
            ", i32 "+load("i32",offsetof(State,visible_mac)));
        const auto ready=value("icmp ne i32 "+value("and i32 "+mask+", 1")+", 0");
        const auto visible=value("select i1 "+ready+", i32 "+load("i32",offsetof(State,pending_mac))+", i32 "+previous);
        pending.push_back({"i32",visible,offsetof(State,visible_mac)});
        const auto previous_status=value("select i1 "+idle+", i32 "+load("i32",offsetof(State,status))+
            ", i32 "+load("i32",offsetof(State,visible_status)));
        const auto published_status=value("call i32 @nt_ee_vector_status_flags(i32 "+load("i32",offsetof(State,pending_mac))+", i32 "+previous_status+")");
        pending.push_back({"i32",value("select i1 "+ready+", i32 "+published_status+", i32 "+previous_status),offsetof(State,visible_status)});
        for (unsigned i=0;i<3;++i)
            pending.push_back({"i32",load("i32",offsetof(State,pending_mac)+(i+1)*4),offsetof(State,pending_mac)+i*4});
        pending.push_back({"i32",value("lshr i32 "+mask+", 1"),offsetof(State,pending_mac_mask)});
        commit();
        advance_visibility(offsetof(State,mac_read_delay));
    }
    void advance_clip() {
        const auto mask=load("i32",offsetof(State,pending_clip_mask));
        const auto idle=value("icmp eq i32 "+mask+", 0");
        const auto previous=value("select i1 "+idle+", i32 "+load("i32",offsetof(State,clip))+
            ", i32 "+load("i32",offsetof(State,visible_clip)));
        const auto ready=value("icmp ne i32 "+value("and i32 "+mask+", 1")+", 0");
        pending.push_back({"i32",value("select i1 "+ready+", i32 "+load("i32",offsetof(State,pending_clip))+", i32 "+previous),offsetof(State,visible_clip)});
        for (unsigned i=0;i<3;++i)
            pending.push_back({"i32",load("i32",offsetof(State,pending_clip)+(i+1)*4),offsetof(State,pending_clip)+i*4});
        pending.push_back({"i32",value("lshr i32 "+mask+", 1"),offsetof(State,pending_clip_mask)});
        commit();
        advance_visibility(offsetof(State,clip_read_delay));
    }
    void clip_result(const std::string& result) {
        pending.push_back({"i32",result,offsetof(State,clip)});
        pending.push_back({"i32",result,offsetof(State,pending_clip)+3*4});
        pending.push_back({"i32",value("or i32 "+load("i32",offsetof(State,pending_clip_mask))+", 8"),offsetof(State,pending_clip_mask)});
        pending.push_back({"i32","4",offsetof(State,clip_read_delay)});
    }
    void commit() {
        for (const auto& store:pending) {
            const auto address=store.address.empty() ? pointer(store.offset) : store.address;
            out<<"  store "<<store.type<<' '<<store.value<<", ptr "<<address<<", align 1\n";
        }
        pending.clear();
    }
private:
    static std::size_t vector_offset(unsigned index,unsigned lane) { return offsetof(State,vectors)+(index-1)*16+lane*4; }
    unsigned next_=0;
};
}
static void emit_region(std::ostream& output,std::span<const Pair> pairs,unsigned unit,std::string_view symbol,bool declarations,bool fallthrough=false) {
    if (unit>1 || pairs.empty() || pairs.size()>2048) throw std::invalid_argument("Invalid VU native region");
    if (symbol.empty() || (symbol.front()>='0' && symbol.front()<='9') ||
        symbol.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_")!=std::string_view::npos)
        throw std::invalid_argument("Invalid VU native symbol");
    const bool graphics_end=pairs.size()>=2 && pairs[pairs.size()-2].end &&
        decode_lower(pairs[pairs.size()-2],unit).operation==LowerOperation::graphics_kick;
    const bool graphics_block=graphics_end || decode_lower(pairs.back(),unit).operation==LowerOperation::graphics_kick;
    if (graphics_block && pairs.back().end) throw std::invalid_argument("Missing XGKICK E delay pair");
    if (!graphics_block && !fallthrough && pairs.size()<2) throw std::invalid_argument("Missing VU terminal delay pair");
    const auto terminal=graphics_block || fallthrough ? Control{} : control(pairs[pairs.size()-2],unit);
    const bool branch_block=terminal.branch!=Branch::none;
    const bool nested_branch=branch_block && direct_delayed_branch(pairs[pairs.size()-2],pairs.back(),unit);
    std::string next_pair,delay_destination;
    std::set<std::string> missing;
    for (const auto& pair:pairs) {
        const auto upper=decode_upper(pair.upper);
        const auto lower=decode_lower(pair,unit);
        if (!upper_supported(upper.operation))
            missing.insert("upper "+std::string(name(upper.operation)));
        if (lower.operation==LowerOperation::unsupported) {
            auto family="lower "+std::string(name(lower.operation));
            if (pair.lower) family+=" (primary="+std::to_string(pair.lower->primary)+
                ", function="+std::to_string(pair.lower->function)+", subfunction="+std::to_string(pair.lower->subfunction)+")";
            missing.insert(family);
        }
    }
    if (!missing.empty()) {
        std::string diagnostic="VU lowering requires operation families:";
        for (const auto& family:missing) diagnostic+="\n  "+family;
        throw std::invalid_argument(diagnostic);
    }
    Writer writer;
    if (declarations) writer.out<<"; AnyPS2 VU ahead-of-time native region\n"
        <<"declare i32 @nt_ee_integer_to_scaled_scalar(i32, i32)\n"
        <<"declare i32 @nt_ee_scaled_scalar_to_integer(i32, i32)\n"
        <<"declare i32 @nt_ee_min_max_scalar(i32, i32, i32)\n"
        <<"declare i64 @nt_ee_add_finite(i32, i32, i32)\n"
        <<"declare i64 @nt_ee_multiply_finite(i32, i32)\n"
        <<"declare i64 @nt_ee_multiply_accumulate_finite(i32, i32, i32, i32)\n"
        <<"declare i32 @nt_ee_vector_lane_flags(i64, i32)\n"
        <<"declare i32 @nt_ee_vector_status_flags(i32, i32)\n"
        <<"declare i64 @nt_vu_divide_finite(i32, i32)\n"
        <<"declare i64 @nt_vu_square_root_finite(i32)\n"
        <<"declare i64 @nt_vu_reciprocal_square_root_finite(i32, i32)\n"
        <<"declare i64 @nt_vu_reciprocal_length_finite(i32, i32, i32)\n"
        <<"declare i64 @nt_vu_sine_finite(i32)\n"
        <<"declare i64 @nt_vu_efu_square_root_finite(i32)\n"
        <<"declare i32 @nt_vu_clip_flags(i32, i32, i32, i32)\n"
        <<"declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)\n";
    writer.out<<"define i32 @"<<symbol<<"(ptr %output) {\nentry:\n"
        <<"  %state = alloca ["<<sizeof(State)<<" x i8], align 8\n"
        <<"  call void @llvm.memcpy.p0.p0.i64(ptr %state, ptr %output, i64 "<<sizeof(State)<<", i1 false)\n";
    for (std::size_t index=0;index<pairs.size();++index) {
        const auto& pair=pairs[index];
        if (pair.address>=(unit ? 2048U : 512U) || (index && pair.address!=pairs[index-1].address+1) ||
            pair.end!=((graphics_end || (!graphics_block && !branch_block && !fallthrough)) && index+2==pairs.size()) || pair.interlock || pair.debug || pair.trap ||
            (control(pair,unit).branch!=Branch::none && (graphics_block || fallthrough ||
                (index+2!=pairs.size() && !(nested_branch && index+1==pairs.size())))))
            throw std::invalid_argument("VU native block requires one terminal branch or E flag and its delay pair");
        const auto upper=decode_upper(pair.upper);
        const auto lower=decode_lower(pair,unit);
        if (graphics_end && index+1==pairs.size() &&
            (lower.operation==LowerOperation::store_quad || lower.operation==LowerOperation::store_integer))
            throw std::invalid_argument("XGKICK E delay-pair data writes require transfer-order analysis");
        const bool quotient_producer=lower.operation==LowerOperation::divide || lower.operation==LowerOperation::square_root ||
            lower.operation==LowerOperation::reciprocal_square_root;
        writer.advance_mac();
        writer.advance_clip();
        // The compiled schedule can make Q available without an explicit WAITQ.
        // Preserve the remaining dependency distance across native block calls.
        const auto q_due=writer.value("icmp eq i32 "+writer.load("i32",offsetof(State,quotient_read_delay))+", 1");
        writer.advance_visibility(offsetof(State,quotient_read_delay));
        writer.out<<"  br i1 "<<q_due<<", label %q_publish"<<index<<", label %q_elapsed"<<index<<"\nq_publish"<<index<<":\n";
        writer.publish_quotient();
        writer.out<<"  br label %q_elapsed"<<index<<"\nq_elapsed"<<index<<":\n";
        // These lower operations synchronize an earlier quotient before this
        // pair reads Q. Unscheduled visibility is deliberately not approximated.
        if (quotient_producer || lower.operation==LowerOperation::wait_quotient) writer.publish_quotient();
        if (lower.operation==LowerOperation::reciprocal_length || lower.operation==LowerOperation::sine ||
            lower.operation==LowerOperation::efu_square_root || lower.operation==LowerOperation::wait_p) writer.publish_p();
        if (upper.operand==Operand::quotient) {
            const auto ready=writer.value("icmp eq i32 "+writer.load("i32",offsetof(State,quotient_pending))+", 0");
            writer.out<<"  br i1 "<<ready<<", label %q_ready"<<index<<", label %schedule_failure\nq_ready"<<index<<":\n";
        }
        if (upper.operation!=Arithmetic::nop && upper.destination==Destination::vector && upper.result && lower.result.bank==Bank::vector &&
            lower.result.index==upper.result)
            throw std::invalid_argument("VU simultaneous vector destinations require explicit priority lowering");
        const auto delayed_register=lower.address_update!=AddressUpdate::none ? lower.address_register :
            lower.operation!=LowerOperation::branch && lower.operation!=LowerOperation::load_integer &&
                lower.operation!=LowerOperation::input_top && lower.operation!=LowerOperation::input_itop &&
                lower.operation!=LowerOperation::mac_and && lower.operation!=LowerOperation::mac_or && lower.operation!=LowerOperation::mac_equal &&
                lower.operation!=LowerOperation::clip_and && lower.operation!=LowerOperation::clip_or && lower.operation!=LowerOperation::clip_equal && lower.operation!=LowerOperation::clip_get &&
                lower.operation!=LowerOperation::status_and && lower.operation!=LowerOperation::status_or && lower.operation!=LowerOperation::status_equal &&
                lower.result.bank==Bank::integer ? lower.result.index : 0U;
        const auto delayed_value=delayed_register ? writer.integer(delayed_register) : "0";
        // Read both sides before committing either result. Dependencies between
        // pairs can use native SSA/loads. Flag reads require proven visibility.
        const bool outer=upper.operation==Arithmetic::outer_multiply || upper.operation==Arithmetic::outer_subtract;
        const bool clipping=upper.operation==Arithmetic::clip;
        if (clipping) {
            if (lower.operation==LowerOperation::clip_set) throw std::invalid_argument("Paired CLIP/FCSET requires explicit priority lowering");
            const auto bits=writer.value("call i32 @nt_vu_clip_flags(i32 "+writer.vector(upper.left,0)+", i32 "+writer.vector(upper.left,1)+
                ", i32 "+writer.vector(upper.left,2)+", i32 "+writer.vector(upper.right,3)+")");
            const auto history=writer.value("shl i32 "+writer.load("i32",offsetof(State,clip))+", 6");
            const auto result=writer.value("and i32 "+writer.value("or i32 "+history+", "+bits)+", 16777215");
            writer.clip_result(result);
        }
        const bool accumulate=upper.operation==Arithmetic::multiply_add || upper.operation==Arithmetic::multiply_subtract || upper.operation==Arithmetic::outer_subtract;
        const bool subtract_product=upper.operation==Arithmetic::multiply_subtract || upper.operation==Arithmetic::outer_subtract;
        const bool arithmetic=outer || accumulate || upper.operation==Arithmetic::add || upper.operation==Arithmetic::subtract || upper.operation==Arithmetic::multiply;
        std::string mac=outer ? writer.value("and i32 "+writer.load("i32",offsetof(State,mac))+", 4369") : "0";
        if (!clipping && upper.operation!=Arithmetic::nop && (upper.result || arithmetic)) for (unsigned lane=0;lane<4;++lane) {
            if (!((outer ? 14U : upper.mask)&(8U>>lane))) continue;
            const auto left=writer.vector(upper.left,outer ? (lane+1)%3 : lane);
            std::string value;
            if (arithmetic) {
                const auto right=outer ? writer.vector(upper.right,(lane+2)%3) : upper.operand==Operand::immediate ? writer.load("i32",offsetof(State,immediate)) :
                    upper.operand==Operand::quotient ? writer.load("i32",offsetof(State,quotient)) :
                    writer.vector(upper.right,upper.operand==Operand::vector ? lane : static_cast<unsigned>(upper.operand)-static_cast<unsigned>(Operand::x));
                const auto accumulator=accumulate ? writer.load("i32",offsetof(State,accumulator)+lane*4) : "0";
                const auto packed=writer.value(accumulate ?
                    "call i64 @nt_ee_multiply_accumulate_finite(i32 "+accumulator+", i32 "+left+", i32 "+right+", i32 "+(subtract_product ? "1" : "0")+")" : (upper.operation==Arithmetic::multiply || outer) ?
                    "call i64 @nt_ee_multiply_finite(i32 "+left+", i32 "+right+")" :
                    "call i64 @nt_ee_add_finite(i32 "+left+", i32 "+right+", i32 "+(upper.operation==Arithmetic::subtract ? "1" : "0")+")");
                const auto unsupported=writer.value("and i64 "+packed+", 8589934592");
                const auto ready=writer.value("icmp eq i64 "+unsupported+", 0");
                const auto label=std::to_string(index)+"_"+std::to_string(lane);
                writer.out<<"  br i1 "<<ready<<", label %math"<<label<<", label %numeric_failure\nmath"<<label<<":\n";
                value=writer.value("trunc i64 "+packed+" to i32");
                const auto flags=writer.value("call i32 @nt_ee_vector_lane_flags(i64 "+packed+", i32 "+std::to_string(lane)+")");
                mac=writer.value("or i32 "+mac+", "+flags);
            } else if (upper.operation==Arithmetic::absolute) value=writer.value("and i32 "+left+", 2147483647");
            else if (upper.operation==Arithmetic::minimum || upper.operation==Arithmetic::maximum) {
                const auto right=upper.operand==Operand::immediate ? writer.load("i32",offsetof(State,immediate)) :
                    writer.vector(upper.right,upper.operand==Operand::vector ? lane : static_cast<unsigned>(upper.operand)-static_cast<unsigned>(Operand::x));
                value=writer.value("call i32 @nt_ee_min_max_scalar(i32 "+left+", i32 "+right+", i32 "+
                    (upper.operation==Arithmetic::maximum ? "1" : "0")+")");
            } else {
                value=writer.value(std::string("call i32 @nt_ee_")+
                    (upper.operation==Arithmetic::integer_to_float ? "integer_to_scaled_scalar" : "scaled_scalar_to_integer")+
                    "(i32 "+left+", i32 "+std::to_string(upper.scale)+")");
            }
            if (upper.destination==Destination::accumulator) writer.pending.push_back({"i32",value,offsetof(State,accumulator)+lane*4});
            else writer.vector_result(upper.result,lane,value);
        }
        if (arithmetic) {
            const auto previous=writer.load("i32",offsetof(State,status));
            const auto status=writer.value("call i32 @nt_ee_vector_status_flags(i32 "+mac+", i32 "+previous+")");
            writer.pending.push_back({"i32",mac,offsetof(State,mac)});
            writer.pending.push_back({"i32",mac,offsetof(State,pending_mac)+3*4});
            writer.pending.push_back({"i32",writer.value("or i32 "+writer.load("i32",offsetof(State,pending_mac_mask))+", 8"),offsetof(State,pending_mac_mask)});
            writer.pending.push_back({"i32",status,offsetof(State,status)});
            writer.pending.push_back({"i32","4",offsetof(State,mac_read_delay)});
        }
        switch (lower.operation) {
        case LowerOperation::status_and: case LowerOperation::status_or: case LowerOperation::status_equal: {
            if (!lower.result.index) break;
            const auto flags=writer.value("and i32 "+writer.load("i32",offsetof(State,visible_status))+", 4095");
            const auto operation=lower.operation==LowerOperation::status_and ? "and" : lower.operation==LowerOperation::status_or ? "or" : "icmp eq";
            const auto value=writer.value(std::string(operation)+" i32 "+flags+", "+std::to_string(lower.literal));
            writer.integer_result(lower.result.index,writer.value(std::string(lower.operation==LowerOperation::status_equal ? "zext i1 " : "trunc i32 ")+value+" to i16"));
            break;
        }
        case LowerOperation::clip_set:
            writer.clip_result(std::to_string(lower.literal));
            break;
        case LowerOperation::clip_get: case LowerOperation::clip_and: case LowerOperation::clip_or: case LowerOperation::clip_equal: {
            if (!lower.result.index) break;
            const auto flags=writer.value("and i32 "+writer.load("i32",offsetof(State,visible_clip))+", 16777215");
            std::string result;
            if (lower.operation==LowerOperation::clip_get)
                result=writer.value("trunc i32 "+writer.value("and i32 "+flags+", 4095")+" to i16");
            else {
                auto compared=flags;
                if (lower.operation!=LowerOperation::clip_equal) compared=writer.value(std::string(lower.operation==LowerOperation::clip_and ? "and" : "or")+" i32 "+flags+", "+std::to_string(lower.literal));
                const auto condition=writer.value(std::string(lower.operation==LowerOperation::clip_and ? "icmp ne" : "icmp eq")+" i32 "+compared+", "+
                    (lower.operation==LowerOperation::clip_and ? "0" : lower.operation==LowerOperation::clip_or ? "16777215" : std::to_string(lower.literal)));
                result=writer.value("zext i1 "+condition+" to i16");
            }
            writer.integer_result(lower.result.index,result);
            break;
        }
        case LowerOperation::graphics_kick: {
            if (!graphics_block || index+(graphics_end ? 2 : 1)!=pairs.size()) throw std::invalid_argument("XGKICK requires a native submission boundary");
            const auto available=writer.value("icmp eq i32 "+writer.load("i32",offsetof(State,graphics_pending))+", 0");
            writer.out<<"  br i1 "<<available<<", label %graphics_ready, label %context_failure\ngraphics_ready:\n";
            const auto address=writer.value("and i16 "+writer.integer(lower.left.index)+", 1023");
            writer.pending.push_back({"i32",writer.value("zext i16 "+address+" to i32"),offsetof(State,graphics_address)});
            writer.pending.push_back({"i32","1",offsetof(State,graphics_pending)});
            writer.pending.push_back({"i32",graphics_end ? "1" : "0",offsetof(State,graphics_completes)});
            next_pair=std::to_string((pair.address+(graphics_end ? 2 : 1))&2047U);
            break;
        }
        case LowerOperation::mac_and: case LowerOperation::mac_or: case LowerOperation::mac_equal: {
            if (!lower.result.index) break;
            const auto flags=writer.value("trunc i32 "+writer.load("i32",offsetof(State,visible_mac))+" to i16");
            const auto operand=writer.integer(lower.left.index);
            const auto value=lower.operation==LowerOperation::mac_equal ?
                writer.value("zext i1 "+writer.value("icmp eq i16 "+flags+", "+operand)+" to i16") :
                writer.value(std::string(lower.operation==LowerOperation::mac_and ? "and" : "or")+" i16 "+flags+", "+operand);
            writer.integer_result(lower.result.index,value);
            break;
        }
        case LowerOperation::wait_p: break;
        case LowerOperation::move_p: {
            if (!lower.result.index || !lower.mask) break;
            const auto ready=writer.value("icmp eq i32 "+writer.load("i32",offsetof(State,p_pending))+", 0");
            writer.out<<"  br i1 "<<ready<<", label %p_ready"<<index<<", label %schedule_failure\np_ready"<<index<<":\n";
            const auto value=writer.load("i32",offsetof(State,scalar_p));
            for (unsigned lane=0;lane<4;++lane) if (lower.mask&(8U>>lane)) writer.vector_result(lower.result.index,lane,value);
            break;
        }
        case LowerOperation::reciprocal_length: case LowerOperation::sine: case LowerOperation::efu_square_root: {
            const auto packed=lower.operation!=LowerOperation::reciprocal_length ?
                writer.value(std::string("call i64 @nt_vu_")+(lower.operation==LowerOperation::sine ? "sine_finite" : "efu_square_root_finite")+
                    "(i32 "+writer.vector(lower.left.index,lower.left.component)+")") :
                writer.value("call i64 @nt_vu_reciprocal_length_finite(i32 "+writer.vector(lower.left.index,0)+
                    ", i32 "+writer.vector(lower.left.index,1)+", i32 "+writer.vector(lower.left.index,2)+")");
            const auto ready=writer.value("icmp eq i64 "+writer.value("and i64 "+packed+", 8589934592")+", 0");
            writer.out<<"  br i1 "<<ready<<", label %p_numeric"<<index<<", label %numeric_failure\np_numeric"<<index<<":\n";
            writer.pending.push_back({"i32",writer.value("trunc i64 "+packed+" to i32"),offsetof(State,pending_p)});
            writer.pending.push_back({"i32","1",offsetof(State,p_pending)});
            break;
        }
        case LowerOperation::input_top: case LowerOperation::input_itop: {
            if (!lower.result.index) break;
            const bool top=lower.operation==LowerOperation::input_top;
            const auto present=writer.value("and i32 "+writer.load("i32",offsetof(State,input_valid))+", "+(top ? "1" : "2"));
            const auto ready=writer.value("icmp ne i32 "+present+", 0");
            writer.out<<"  br i1 "<<ready<<", label %input"<<index<<", label %context_failure\ninput"<<index<<":\n";
            writer.integer_result(lower.result.index,writer.load("i16",top ? offsetof(State,input_top) : offsetof(State,input_itop)));
            break;
        }
        case LowerOperation::load_quad: case LowerOperation::store_quad:
        case LowerOperation::load_integer: case LowerOperation::store_integer: {
            const bool integer=lower.operation==LowerOperation::load_integer || lower.operation==LowerOperation::store_integer;
            const bool load=lower.operation==LowerOperation::load_quad || lower.operation==LowerOperation::load_integer;
            const auto base=writer.integer(lower.address_register);
            auto address=base;
            if (lower.address_update==AddressUpdate::decrement && lower.address_register)
                address=writer.value("sub i16 "+base+", 1");
            else if (lower.offset) address=writer.value("add i16 "+base+", "+std::to_string(lower.offset));
            if (lower.mask && (!load || lower.result.index)) {
                if (!unit) {
                    const auto mapped=writer.value("and i16 "+address+", 1024");
                    const auto ready=writer.value("icmp eq i16 "+mapped+", 0");
                    writer.out<<"  br i1 "<<ready<<", label %memory"<<index<<", label %memory_failure\nmemory"<<index<<":\n";
                }
                const auto wrapped=writer.value("and i16 "+address+", "+(unit ? "1023" : "255"));
                const auto byte_offset=writer.value("shl i64 "+writer.value("zext i16 "+wrapped+" to i64")+", 4");
                const auto start=writer.value("getelementptr i8, ptr "+writer.pointer(offsetof(State,data))+", i64 "+byte_offset);
                for (unsigned lane=0;lane<4;++lane) if (lower.mask&(8U>>lane)) {
                    const auto pointer=writer.value("getelementptr i8, ptr "+start+", i64 "+std::to_string(lane*4));
                    if (load) {
                        const auto value=writer.value("load i32, ptr "+pointer+", align 1");
                        if (integer) writer.integer_result(lower.result.index,writer.value("trunc i32 "+value+" to i16"));
                        else writer.vector_result(lower.result.index,lane,value);
                    } else {
                        const auto value=integer ? writer.value("zext i16 "+writer.integer(lower.left.index)+" to i32") : writer.vector(lower.left.index,lane);
                        writer.pending.push_back({"i32",value,0,pointer});
                    }
                }
            }
            if (lower.address_update!=AddressUpdate::none)
                writer.integer_result(lower.address_register,lower.address_update==AddressUpdate::increment ? writer.value("add i16 "+base+", 1") : address);
            break;
        }
        case LowerOperation::wait_quotient: break;
        case LowerOperation::divide: case LowerOperation::square_root: case LowerOperation::reciprocal_square_root: {
            const auto right=writer.vector(lower.right.index,lower.right.component);
            const auto packed=lower.operation==LowerOperation::square_root ?
                writer.value("call i64 @nt_vu_square_root_finite(i32 "+right+")") :
                writer.value(std::string("call i64 @nt_vu_")+(lower.operation==LowerOperation::divide ? "divide_finite" : "reciprocal_square_root_finite")+
                    "(i32 "+writer.vector(lower.left.index,lower.left.component)+", i32 "+right+")");
            const auto ready=writer.value("icmp sge i64 "+packed+", 0");
            writer.out<<"  br i1 "<<ready<<", label %q_numeric"<<index<<", label %numeric_failure\nq_numeric"<<index<<":\n";
            writer.pending.push_back({"i32",writer.value("trunc i64 "+packed+" to i32"),offsetof(State,pending_quotient)});
            writer.pending.push_back({"i32",writer.value("trunc i64 "+writer.value("lshr i64 "+packed+", 32")+" to i32"),offsetof(State,quotient_flags)});
            writer.pending.push_back({"i32","1",offsetof(State,quotient_pending)});
            writer.pending.push_back({"i32",lower.operation==LowerOperation::reciprocal_square_root ? "13" : "7",offsetof(State,quotient_read_delay)});
            break;
        }
        case LowerOperation::branch: {
            const auto& transfer=lower.transfer;
            const bool delayed=nested_branch && index+1==pairs.size();
            if (delayed) delay_destination=next_pair;
            const auto branch_register=[&](unsigned reg) {
                if (!reg) return std::string("0");
                if (delayed) return writer.integer(reg);
                const auto delayed=writer.load("i16",offsetof(State,delayed_integer));
                const auto backup=writer.load("i16",offsetof(State,delayed_value));
                const auto matches=writer.value("icmp eq i16 "+delayed+", "+std::to_string(reg));
                const auto current=writer.integer(reg);
                return writer.value("select i1 "+matches+", i16 "+backup+", i16 "+current);
            };
            const auto address_mask=unit ? 2047U : 511U;
            const auto continuation=delayed ?
                writer.value("and i32 "+writer.value("add i32 "+delay_destination+", 1")+", "+std::to_string(address_mask)) :
                std::to_string((pair.address+2)&address_mask);
            if (transfer.target) next_pair=std::to_string(*transfer.target);
            else next_pair=writer.value("and i32 "+writer.value("zext i16 "+branch_register(transfer.source)+" to i32")+", "+std::to_string(address_mask));
            const char* predicate=nullptr;
            switch (transfer.branch) {
            case Branch::equal: predicate="eq"; break;
            case Branch::not_equal: predicate="ne"; break;
            case Branch::less_zero: predicate="slt"; break;
            case Branch::greater_zero: predicate="sgt"; break;
            case Branch::less_equal_zero: predicate="sle"; break;
            case Branch::greater_equal_zero: predicate="sge"; break;
            default: break;
            }
            if (predicate) {
                const auto left=branch_register(transfer.source);
                const auto right=transfer.branch==Branch::equal || transfer.branch==Branch::not_equal ? branch_register(transfer.target_register) : "0";
                const auto taken=writer.value(std::string("icmp ")+predicate+" i16 "+left+", "+right);
                next_pair=writer.value("select i1 "+taken+", i32 "+next_pair+", i32 "+continuation);
            }
            if (transfer.branch==Branch::call || transfer.branch==Branch::register_call)
                writer.integer_result(transfer.link_register,continuation);
            break;
        }
        case LowerOperation::literal:
            writer.pending.push_back({"i32",std::to_string(lower.literal),offsetof(State,immediate)}); break;
        case LowerOperation::integer_add: case LowerOperation::integer_subtract:
        case LowerOperation::integer_and: case LowerOperation::integer_or: {
            const auto operation=lower.operation==LowerOperation::integer_add ? "add" :
                lower.operation==LowerOperation::integer_subtract ? "sub" : lower.operation==LowerOperation::integer_and ? "and" : "or";
            writer.integer_result(lower.result.index,writer.value(std::string(operation)+" i16 "+writer.integer(lower.left.index)+", "+writer.integer(lower.right.index))); break;
        }
        case LowerOperation::integer_add_immediate: case LowerOperation::integer_subtract_immediate:
            writer.integer_result(lower.result.index,writer.value(std::string(lower.operation==LowerOperation::integer_add_immediate ? "add" : "sub")+
                " i16 "+writer.integer(lower.left.index)+", "+std::to_string(lower.offset))); break;
        case LowerOperation::move: case LowerOperation::rotate:
            for (unsigned lane=0;lane<4;++lane) if (lower.mask&(8U>>lane))
                writer.vector_result(lower.result.index,lane,writer.vector(lower.left.index,lower.operation==LowerOperation::rotate ? (lane+1)%4 : lane));
            break;
        case LowerOperation::vector_to_integer:
            writer.integer_result(lower.result.index,writer.value("trunc i32 "+writer.vector(lower.left.index,lower.left.component)+" to i16")); break;
        case LowerOperation::integer_to_vector: {
            const auto value=writer.value("sext i16 "+writer.integer(lower.left.index)+" to i32");
            for (unsigned lane=0;lane<4;++lane) if (lower.mask&(8U>>lane)) writer.vector_result(lower.result.index,lane,value);
            break;
        }
        default: throw std::invalid_argument("Unsupported VU lower native operation");
        }
        writer.pending.push_back({"i16",std::to_string(delayed_register),offsetof(State,delayed_integer)});
        writer.pending.push_back({"i16",delayed_value,offsetof(State,delayed_value)});
        writer.commit();
    }
    if (nested_branch) {
        const auto call_delay=[&](unsigned address) {
            return writer.value("call i32 @"+std::string(symbol)+"_delay_"+std::to_string(address)+"(ptr %state)");
        };
        std::string result;
        if (terminal.branch==Branch::jump) result=call_delay(*terminal.target);
        else {
            const auto taken=writer.value("icmp eq i32 "+delay_destination+", "+std::to_string(*terminal.target));
            writer.out<<"  br i1 "<<taken<<", label %delay_taken, label %delay_other\ndelay_taken:\n";
            const auto yes=call_delay(*terminal.target);
            writer.out<<"  br label %delay_done\ndelay_other:\n";
            const auto no=call_delay((pairs[pairs.size()-2].address+2)&(unit ? 2047U : 511U));
            writer.out<<"  br label %delay_done\ndelay_done:\n";
            result=writer.value("phi i32 [ "+yes+", %delay_taken ], [ "+no+", %delay_other ]");
        }
        const auto succeeded=writer.value("icmp eq i32 "+result+", 2");
        writer.out<<"  br i1 "<<succeeded<<", label %delay_commit, label %delay_failed\ndelay_failed:\n  ret i32 "<<result<<"\ndelay_commit:\n";
    }
    if (!branch_block && !graphics_block)
        next_pair=std::to_string((pairs.back().address+1)&(unit ? 2047U : 511U));
    writer.pending.push_back({"i32",next_pair,offsetof(State,next_pair)}); writer.commit();
    if ((!branch_block && !graphics_block && !fallthrough) || graphics_end) {
        writer.publish_quotient(); writer.publish_p();
        writer.pending.push_back({"i32",writer.load("i32",offsetof(State,mac)),offsetof(State,visible_mac)});
        writer.pending.push_back({"i32",writer.load("i32",offsetof(State,status)),offsetof(State,visible_status)});
        writer.pending.push_back({"i32",writer.load("i32",offsetof(State,clip)),offsetof(State,visible_clip)});
        writer.pending.push_back({"i32","0",offsetof(State,pending_clip_mask)});
        writer.pending.push_back({"i32","0",offsetof(State,pending_mac_mask)});
        writer.pending.push_back({"i32","0",offsetof(State,mac_read_delay)}); writer.commit();
        writer.pending.push_back({"i32","0",offsetof(State,clip_read_delay)}); writer.commit();
    }
    writer.out<<"  call void @llvm.memcpy.p0.p0.i64(ptr %output, ptr %state, i64 "<<sizeof(State)<<", i1 false)\n"
        <<"  ret i32 "<<(graphics_block ? 6 : (branch_block || fallthrough) ? 2 : 0)<<"\nnumeric_failure:\n  ret i32 1\nschedule_failure:\n  ret i32 3\nmemory_failure:\n  ret i32 4\ncontext_failure:\n  ret i32 5\n}\n";
    output<<writer.out.str();
}
void emit_llvm(std::ostream& output,std::span<const Pair> pairs,unsigned unit,std::string_view symbol) {
    emit_region(output,pairs,unit,symbol,true);
}
static std::size_t emit_program_module(std::ostream& output,std::span<const Pair> pairs,unsigned unit,
                       std::span<const unsigned> entries,std::string_view symbol,unsigned continuation_depth,bool all_entries,bool declarations) {
    if (pairs.empty()) throw std::invalid_argument("Empty VU program image");
    for (std::size_t i=1;i<pairs.size();++i)
        if (pairs[i].address!=pairs[i-1].address+1) throw std::invalid_argument("VU program image must be contiguous");
    if (continuation_depth>2048) throw std::invalid_argument("VU continuation depth exceeds 2048");
    if (all_entries && continuation_depth) throw std::invalid_argument("Whole VU conversion cannot be combined with continuation discovery");
    std::set<unsigned> roots(entries.begin(),entries.end());
    if (all_entries) for (const auto& pair:pairs) roots.insert(pair.address);
    Flow flow;
    for (unsigned depth=0;;++depth) {
        const std::vector<unsigned> seeds(roots.begin(),roots.end());
        flow=discover(pairs,unit,seeds);
        if ((!all_entries && !flow.missing.empty()) || !flow.complex_delay.empty()) {
            std::string message="VU program control-flow gaps at continuation depth "+std::to_string(depth)+":";
            for (const auto address:flow.missing) message+="\n  missing pair "+std::to_string(address);
            for (const auto address:flow.complex_delay) message+="\n  unsupported delay at pair "+std::to_string(address);
            throw std::invalid_argument(message);
        }
        if (depth==continuation_depth) break;
        const auto previous_size=roots.size();
        for (const auto& transfer:flow.transfers) if (transfer.terminates)
            roots.insert((transfer.address+2)&(unit ? 2047U : 511U));
        if (roots.size()==previous_size) break;
    }
    for (const auto& pair:pairs) if (!all_entries && !pair.end && flow.reachable.contains(pair.address) && decode_lower(pair,unit).operation==LowerOperation::graphics_kick)
        roots.insert((pair.address+1)&(unit ? 2047U : 511U));
    if (!all_entries) roots.insert(flow.delayed_continuations.begin(),flow.delayed_continuations.end());
    for (const auto& transfer:flow.transfers) {
        if (all_entries) continue; // All in-image entries already exist; external targets remain explicit stops.
        if (transfer.terminates) continue;
        if (transfer.control.target) roots.insert(*transfer.control.target);
        const auto branch=transfer.control.branch;
        if (branch!=Branch::jump && branch!=Branch::register_jump)
            roots.insert((transfer.address+2)&(unit ? 2047U : 511U));
    }
    // Build transactionally: a late unsupported block must not leave a partial module.
    std::ostringstream module;
    for (const auto root:roots) {
        const auto found=std::find_if(pairs.begin(),pairs.end(),[&](const Pair& pair) { return pair.address==root; });
        if (found==pairs.end()) throw std::invalid_argument("Missing VU block entry");
        auto end=found;
        while (end!=pairs.end() && !end->end && control(*end,unit).branch==Branch::none &&
               decode_lower(*end,unit).operation!=LowerOperation::graphics_kick) ++end;
        const bool fallthrough=end==pairs.end();
        if (fallthrough && !all_entries) throw std::invalid_argument("Missing VU block terminator");
        const bool graphics=!fallthrough && !end->end && decode_lower(*end,unit).operation==LowerOperation::graphics_kick;
        if (!fallthrough && !graphics && end+1==pairs.end()) throw std::invalid_argument("Missing VU delay pair");
        try {
            if (!fallthrough && !graphics && direct_delayed_branch(*end,*(end+1),unit)) {
                const auto transfer=control(*end,unit);
                std::set<unsigned> destinations{*transfer.target};
                if (transfer.branch!=Branch::jump) destinations.insert((end->address+2)&(unit ? 2047U : 511U));
                for (const auto destination:destinations) {
                    const auto slot=std::find_if(pairs.begin(),pairs.end(),[&](const Pair& pair) { return pair.address==destination; });
                    if (slot==pairs.end() || slot->end || control(*slot,unit).branch!=Branch::none ||
                        decode_lower(*slot,unit).operation==LowerOperation::graphics_kick)
                        throw std::invalid_argument("Unsupported chained VU delay continuation");
                    emit_region(module,std::span<const Pair>(&*slot,1),unit,
                        std::string(symbol)+"_"+std::to_string(root)+"_delay_"+std::to_string(destination),declarations,true);
                    declarations=false;
                }
            }
            emit_region(module,std::span<const Pair>(found,fallthrough ? end : end+(graphics ? 1 : 2)),unit,std::string(symbol)+"_"+std::to_string(root),declarations,fallthrough);
        } catch (const std::invalid_argument& error) {
            throw std::invalid_argument("VU block at pair "+std::to_string(root)+": "+error.what());
        }
        declarations=false;
    }
    module<<"@"<<symbol<<"_table = private constant ["<<roots.size()<<" x { i32, ptr }] [\n";
    std::size_t index=0;
    for (const auto root:roots)
        module<<"  { i32, ptr } { i32 "<<root<<", ptr @"<<symbol<<"_"<<root<<" }"<<(++index==roots.size() ? "\n" : ",\n");
    module<<"]\ndefine ptr @"<<symbol<<"_blocks() {\n  ret ptr @"<<symbol<<"_table\n}\n"
        <<"define i32 @"<<symbol<<"_count() {\n  ret i32 "<<roots.size()<<"\n}\n";
    module<<"@"<<symbol<<"_identity = private constant ["<<pairs.size()*8<<" x i8] [";
    bool first_byte=true;
    for (const auto& pair:pairs) {
        const auto lower=pair.immediate ? *pair.immediate : pair.lower ? pair.lower->encoding : 0U;
        for (const auto word:{lower,pair.upper.encoding}) for (unsigned byte=0;byte<4;++byte) {
            if (!first_byte) module<<", ";
            first_byte=false;
            module<<"i8 "<<((word>>(byte*8))&255U);
        }
    }
    module<<"]\ndefine ptr @"<<symbol<<"_image() {\n  ret ptr @"<<symbol<<"_identity\n}\n"
        <<"define i32 @"<<symbol<<"_size() {\n  ret i32 "<<pairs.size()*8<<"\n}\n"
        <<"define i32 @"<<symbol<<"_destination() {\n  ret i32 "<<pairs.front().address<<"\n}\n"
        <<"define i32 @"<<symbol<<"_unit() {\n  ret i32 "<<unit<<"\n}\n";
    output<<module.str();
    return roots.size();
}
namespace {
struct ArtifactDescription { std::string symbol; std::size_t count; unsigned destination; std::size_t bytes; };
void emit_catalog(std::ostream& output,std::span<const ArtifactDescription> artifacts,std::string_view symbol) {
    constexpr auto type="{ ptr, i32, i32, ptr, i32 }";
    output<<"@"<<symbol<<"_catalog = private constant ["<<artifacts.size()<<" x "<<type<<"] [\n";
    for (std::size_t i=0;i<artifacts.size();++i) {
        const auto& artifact=artifacts[i];
        output<<"  "<<type<<" { ptr @"<<artifact.symbol<<"_table, i32 "<<artifact.count
            <<", i32 "<<artifact.destination<<", ptr @"<<artifact.symbol<<"_identity, i32 "<<artifact.bytes<<" }"
            <<(i+1==artifacts.size() ? "\n" : ",\n");
    }
    output<<"]\ndefine ptr @"<<symbol<<"_artifacts() {\n  ret ptr @"<<symbol<<"_catalog\n}\n"
        <<"define i32 @"<<symbol<<"_artifact_count() {\n  ret i32 "<<artifacts.size()<<"\n}\n";
}
}
void emit_program_llvm(std::ostream& output,std::span<const Pair> pairs,unsigned unit,
                       std::span<const unsigned> entries,std::string_view symbol,unsigned continuation_depth,bool all_entries) {
    const auto count=emit_program_module(output,pairs,unit,entries,symbol,continuation_depth,all_entries,true);
    const ArtifactDescription description{std::string(symbol),count,pairs.front().address,pairs.size()*8};
    emit_catalog(output,std::span(&description,1),symbol);
}
void emit_catalog_llvm(std::ostream& output,std::span<const std::vector<Pair>> images,unsigned unit,std::string_view symbol) {
    if (images.empty() || images.size()>4096) throw std::invalid_argument("Invalid VU image catalog size");
    std::ostringstream module;
    std::vector<ArtifactDescription> descriptions;
    for (std::size_t i=0;i<images.size();++i) {
        const auto name=std::string(symbol)+"_artifact_"+std::to_string(i);
        const auto count=emit_program_module(module,images[i],unit,{},name,0,true,i==0);
        descriptions.push_back({name,count,images[i].front().address,images[i].size()*8});
    }
    emit_catalog(module,descriptions,symbol);
    module<<"define i32 @"<<symbol<<"_unit() {\n  ret i32 "<<unit<<"\n}\n";
    output<<module.str();
}
}
