#include <nt/element_range.hpp>
#include <nt/vu_lower.hpp>
#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <stdexcept>

namespace nt::render {
ElementRangeGuard analyze_element_range(std::span<const vu::Pair> pairs,unsigned unit) {
    const auto structure=analyze_loop_structure(pairs,unit);
    const auto recovery=recover_pipelined_loop(pairs,unit,structure);
    if (!recovery.loop) throw std::invalid_argument("Element range requires a recovered loop: "+recovery.reason);
    const auto& loop=*recovery.loop;
    if (structure.exit_branch!=vu::Branch::not_equal || !structure.counter)
        throw std::invalid_argument("Element range requires an affine countdown-to-zero backedge");
    const auto counter=*structure.counter;
    const auto step=structure.steps[counter];
    const bool left=structure.compare_left==counter && structure.compare_right==0;
    const bool right=structure.compare_right==counter && structure.compare_left==0;
    const auto compared=left ? structure.compared_left : structure.compared_right;
    if ((!left && !right) || step>=0 || !compared.known || compared.base!=int(counter) || compared.offset!=step)
        throw std::invalid_argument("Element range requires the updated countdown at the backedge");
    // The branch is the second-to-last pair. When the pair before it updates the
    // counter, the comparison reads the backed-up pre-update value.
    if (pairs.size()<3) throw std::invalid_argument("Element range requires a branch and its delay slot after the body");
    const unsigned trip_offset=vu::branch_backup_register(vu::decode_lower(pairs[pairs.size()-3],unit))==counter ? 1U : 0U;
    for (unsigned reg=1;reg<16;++reg)
        if (!structure.affine_update[reg])
            throw std::invalid_argument("Element range requires affine integer recurrences");
    ElementRangeGuard guard;
    guard.rows=unit ? 1024:256; guard.counter=counter; guard.step=static_cast<unsigned>(-std::int64_t(step)); guard.minimum_element=loop.depth;
    guard.trip_offset=trip_offset;
    guard.steps=structure.steps; guard.integer_writes=structure.integer_writes;
    std::set<std::tuple<unsigned,std::int32_t,unsigned>> written;
    for (const auto& store:loop.stores) {
        if (store.base==0 || store.base>=16 || store.stride<=0 || store.lane>=4)
            throw std::invalid_argument("Element range requires forward output streams");
        auto stream=std::find_if(guard.outputs.begin(),guard.outputs.end(),[&](const auto& output){return output.base==store.base;});
        if (stream==guard.outputs.end()) stream=guard.outputs.insert(guard.outputs.end(),{store.base,store.row,store.row,store.stride});
        if (stream->stride!=store.stride || !written.emplace(store.base,store.row,store.lane).second)
            throw std::invalid_argument("Element outputs overlap or use different strides on one stream");
        stream->first=std::min(stream->first,store.row); stream->last=std::max(stream->last,store.row);
    }
    if (guard.outputs.empty() || guard.outputs.size()>8) throw std::invalid_argument("Element range requires one to eight output streams");
    for (const auto& output:guard.outputs)
        if (std::int64_t(output.last)-output.first>=output.stride)
            throw std::invalid_argument("Element output records overlap across iterations");
    for (const auto& write:guard.integer_writes)
        if (!write.known || write.base < -1 || write.base>17)
            throw std::invalid_argument("Element integer range is not affine");
    for (const auto& access:structure.accesses) {
        if (access.kind==AccessKind::store_integer) throw std::invalid_argument("Element integer stores are not replaceable");
        if (!access.address.known) throw std::invalid_argument("Element memory range is not affine");
        if (access.kind==AccessKind::load_vector || access.kind==AccessKind::load_integer) {
            ElementRangeGuard::Range read{access.address.base,access.address.offset,access.stride,false};
            for (std::size_t i=0;i<guard.outputs.size();++i) {
                std::vector<const MemoryAccess*> stores;
                for (const auto& store:structure.accesses)
                    if (store.kind==AccessKind::store_vector && store.address.known && store.address.base==int(guard.outputs[i].base))
                        stores.push_back(&store);
                if (stores.empty() || read.stride<=0 || std::any_of(stores.begin(),stores.end(),[&](const auto* store){return store->stride!=read.stride;}))
                    continue;
                read.ordered|=1U<<i;
                for (const auto* store:stores)
                    guard.orders.push_back({read.base,store->address.base,read.offset,store->address.offset,read.stride,
                        access.pair<store->pair,static_cast<unsigned>(guard.reads.size()),static_cast<unsigned>(i)});
            }
            guard.reads.push_back(read);
        }
    }
    // Recovered prefetches may address earlier iterations. Guard those too,
    // using only the requested steady-state subrange, not an invented prologue.
    for (const auto& node:loop.nodes) if (node.kind==ElementNodeKind::load) {
        if (node.a>18) throw std::invalid_argument("Invalid recovered load base");
        const int base=node.a==16 ? -1 : node.a>=17 ? int(node.a)-1 : int(node.a);
        ElementRangeGuard::Range read{base,static_cast<std::int32_t>(node.b),node.a<16 ? loop.strides[node.a]:0,true};
        // A recovered prefetch re-expresses a physical load stream; it shares
        // that stream's ordering exemptions.
        for (const auto& physical:guard.reads)
            if (!physical.selected && physical.base==read.base && physical.stride==read.stride) read.ordered|=physical.ordered;
        guard.reads.push_back(read);
    }
    return guard;
}
namespace {
class Emitter {
public:
    explicit Emitter(std::ostream& out):out(out) {}
    std::string value(const std::string& expression) {
        const auto name="%g"+std::to_string(next++); out<<"  "<<name<<" = "<<expression<<"\n"; return name;
    }
    std::string parameter(unsigned index) {
        const auto pointer=value("getelementptr i32, ptr %parameters, i32 "+std::to_string(index));
        return value("zext i32 "+value("load i32, ptr "+pointer+", align 4")+" to i64");
    }
    std::string base(int index) {
        if (index==-1 || index==0) return "0";
        return parameter(index>=16 ? unsigned(index)+3 : unsigned(index)+2);
    }
    void require(const std::string& condition,unsigned failure) {
        const auto label="ok"+std::to_string(next++);
        out<<"  br i1 "<<condition<<", label %"<<label<<", label %fail"<<failure<<"\n"<<label<<":\n";
    }
    std::string add(const std::string& a,const std::string& b) { return value("add i64 "+a+", "+b); }
    std::string multiply(const std::string& a,std::int32_t b) { return value("mul i64 "+a+", "+std::to_string(b)); }
    void bound(const std::string& a,unsigned maximum) { require(value("icmp ule i64 "+a+", "+std::to_string(maximum)),3); }
    std::ostream& out;
    unsigned next=0;
};
}
void emit_element_range_llvm(std::ostream& out,const ElementRangeGuard& guard,std::string_view symbol) {
    const auto letter=[](char c) { return (c>='a' && c<='z') || (c>='A' && c<='Z') || c=='_'; };
    if (symbol.empty() || !letter(symbol.front()) || std::any_of(symbol.begin(),symbol.end(),[&](char c) {
            return !letter(c) && !(c>='0' && c<='9'); })) throw std::invalid_argument("Invalid range-guard LLVM symbol");
    if (!guard.step || guard.counter==0 || guard.counter>=16 || guard.trip_offset>1 || (guard.rows!=256 && guard.rows!=1024))
        throw std::invalid_argument("Invalid element range guard");
    out<<"define i32 @"<<symbol<<"(ptr readonly %parameters) {\nentry:\n";
    Emitter e(out);
    const auto count=e.parameter(0),first=e.parameter(1),counter=e.base(int(guard.counter));
    e.require(e.value("icmp ne i64 "+count+", 0"),1);
    // A zero counter terminates only when the backed-up value is compared.
    if (!guard.trip_offset) e.require(e.value("icmp ne i64 "+counter+", 0"),1);
    const auto remainder=e.value("urem i64 "+counter+", "+std::to_string(guard.step));
    e.require(e.value("icmp eq i64 "+remainder+", 0"),1);
    const auto iterations=e.add(e.value("udiv i64 "+counter+", "+std::to_string(guard.step)),std::to_string(guard.trip_offset));
    e.require(e.value("icmp ule i64 "+iterations+", "+std::to_string(guard.rows)),1);
    e.require(e.value("icmp uge i64 "+first+", "+std::to_string(guard.minimum_element)),2);
    e.require(e.value("icmp ule i64 "+e.add(first,count)+", "+iterations),1);
    const auto last=e.add(iterations,"-1"),selected_last=e.add(e.add(first,count),"-1");
    // Signed arithmetic in i64 preserves negative offsets without host UB.
    // An unsigned bound test rejects both a negative result and an overflowed VI.
    for (unsigned reg=1;reg<16;++reg) {
        const auto initial=e.base(int(reg)); e.bound(initial,65535);
        // With the backup the final update takes the counter below zero; that
        // 16-bit wrap is executed by the original replayed final iteration.
        const bool final_wrap=guard.trip_offset && reg==guard.counter;
        e.bound(e.add(initial,e.multiply(final_wrap ? last:iterations,guard.steps[reg])),65535);
    }
    for (const auto& write:guard.integer_writes) {
        const auto initial=e.add(e.base(write.base),std::to_string(write.offset));
        const auto step=write.base>0 && write.base<16 ? guard.steps[write.base]:0;
        if (guard.trip_offset && write.base==int(guard.counter)) {
            // Counter-derived values of the final iteration go below zero; that
            // iteration is replayed by the original code. Bound the earlier ones.
            const auto earlier=e.value("icmp ugt i64 "+iterations+", 1");
            e.bound(e.value("select i1 "+earlier+", i64 "+initial+", i64 0"),65535);
            e.bound(e.value("select i1 "+earlier+", i64 "+e.add(initial,e.multiply(e.add(last,"-1"),step))+", i64 0"),65535);
            continue;
        }
        e.bound(initial,65535);
        e.bound(e.add(initial,e.multiply(last,step)),65535);
    }
    if (guard.outputs.empty()) throw std::invalid_argument("Element range guard has no output stream");
    std::vector<std::pair<std::string,std::string>> outputs;
    for (const auto& output:guard.outputs) {
        const auto first_row=e.add(e.base(int(output.base)),std::to_string(output.first));
        const auto last_row=e.add(e.add(e.base(int(output.base)),std::to_string(output.last)),e.multiply(last,output.stride));
        e.bound(first_row,guard.rows-1); e.bound(last_row,guard.rows-1);
        // Streams must not write each other's rows.
        for (const auto& [other_first,other_last]:outputs) {
            const auto before=e.value("icmp ult i64 "+last_row+", "+other_first);
            const auto after=e.value("icmp ugt i64 "+first_row+", "+other_last);
            e.require(e.value("or i1 "+before+", "+after),4);
        }
        outputs.push_back({first_row,last_row});
    }
    // Disjointness of each ordered read from each stream, combined with its order checks below.
    std::map<std::pair<unsigned,unsigned>,std::string> disjoint;
    for (std::size_t index=0;index<guard.reads.size();++index) {
        const auto& read=guard.reads[index];
        const auto origin=e.add(e.base(read.base),std::to_string(read.offset));
        const auto begin=e.add(origin,e.multiply(read.selected ? first:"0",read.stride));
        const auto end=e.add(origin,e.multiply(read.selected ? selected_last:last,read.stride));
        e.bound(begin,guard.rows-1); e.bound(end,guard.rows-1);
        const auto& low=read.stride<0 ? end:begin;
        const auto& high=read.stride<0 ? begin:end;
        for (std::size_t i=0;i<outputs.size();++i) {
            const auto before=e.value("icmp ult i64 "+high+", "+outputs[i].first);
            const auto after=e.value("icmp ugt i64 "+low+", "+outputs[i].second);
            const auto apart=e.value("or i1 "+before+", "+after);
            if ((read.ordered>>i)&1U) disjoint[{unsigned(index),unsigned(i)}]=apart;
            else e.require(apart,4);
        }
    }
    for (const auto& order:guard.orders) {
        const auto distance=e.value("sub i64 "+e.add(e.base(order.load_base),std::to_string(order.load_offset))+", "+
            e.add(e.base(order.store_base),std::to_string(order.store_offset)));
        const auto stride=std::to_string(order.stride);
        const auto apart=e.value("icmp ne i64 "+e.value("srem i64 "+distance+", "+stride)+", 0");
        const auto later=e.value("icmp sge i64 "+distance+", "+stride);
        const auto same=order.load_first ? e.value("icmp eq i64 "+distance+", 0") : std::string("false");
        const auto ordered=e.value("or i1 "+e.value("or i1 "+apart+", "+later)+", "+same);
        const auto found=disjoint.find({order.read,order.output});
        if (found==disjoint.end()) throw std::logic_error("Element order lacks its read range");
        e.require(e.value("or i1 "+found->second+", "+ordered),4);
    }
    out<<"  ret i32 0\n";
    for (unsigned code=1;code<=4;++code) out<<"fail"<<code<<":\n  ret i32 "<<code<<"\n";
    out<<"}\n";
    if (!out) throw std::runtime_error("Cannot emit element range guard");
}
}
