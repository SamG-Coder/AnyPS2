#include <nt/element_range.hpp>
#include <nt/vu_lower.hpp>
#include <algorithm>
#include <set>
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
    // Keep branch sampling out of the integer-result latency window. The
    // accepted form matches the existing bounded-loop frontend's separation.
    if (pairs.size()<5) throw std::invalid_argument("Countdown lacks branch separation");
    for (std::size_t i=pairs.size()-4;i<pairs.size()-2;++i) {
        const auto lower=vu::decode_lower(pairs[i],unit);
        if ((lower.result.bank==vu::Bank::integer && lower.result.index==counter) ||
            (lower.address_update!=vu::AddressUpdate::none && lower.address_register==counter))
            throw std::invalid_argument("Countdown lacks branch separation");
    }
    for (unsigned reg=1;reg<16;++reg)
        if (!structure.affine_update[reg])
            throw std::invalid_argument("Element range requires affine integer recurrences");
    ElementRangeGuard guard;
    guard.rows=unit ? 1024:256; guard.counter=counter; guard.step=static_cast<unsigned>(-std::int64_t(step)); guard.minimum_element=loop.depth;
    guard.steps=structure.steps; guard.integer_writes=structure.integer_writes;
    const auto& first=loop.stores.front();
    guard.output_base=first.base; guard.output_first=guard.output_last=first.row; guard.output_stride=first.stride;
    if (first.base==0 || first.base>=16 || first.stride<=0)
        throw std::invalid_argument("Element range requires a forward output stream");
    std::set<std::pair<std::int32_t,unsigned>> written;
    for (const auto& store:loop.stores) {
        if (store.base!=first.base || store.stride!=first.stride || store.lane>=4 ||
            !written.emplace(store.row,store.lane).second)
            throw std::invalid_argument("Element outputs overlap or use different streams");
        guard.output_first=std::min(guard.output_first,store.row); guard.output_last=std::max(guard.output_last,store.row);
    }
    if (std::int64_t(guard.output_last)-guard.output_first>=first.stride)
        throw std::invalid_argument("Element output records overlap across iterations");
    for (const auto& write:guard.integer_writes)
        if (!write.known || write.base < -1 || write.base>17)
            throw std::invalid_argument("Element integer range is not affine");
    for (const auto& access:structure.accesses) {
        if (access.kind==AccessKind::store_integer) throw std::invalid_argument("Element integer stores are not replaceable");
        if (!access.address.known) throw std::invalid_argument("Element memory range is not affine");
        if (access.kind==AccessKind::load_vector || access.kind==AccessKind::load_integer)
            guard.reads.push_back({access.address.base,access.address.offset,access.stride,false});
    }
    // Recovered prefetches may address earlier iterations. Guard those too,
    // using only the requested steady-state subrange, not an invented prologue.
    for (const auto& node:loop.nodes) if (node.kind==ElementNodeKind::load) {
        if (node.a>18) throw std::invalid_argument("Invalid recovered load base");
        const int base=node.a==16 ? -1 : node.a>=17 ? int(node.a)-1 : int(node.a);
        guard.reads.push_back({base,static_cast<std::int32_t>(node.b),node.a<16 ? loop.strides[node.a]:0,true});
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
    if (!guard.step || guard.counter==0 || guard.counter>=16 || (guard.rows!=256 && guard.rows!=1024))
        throw std::invalid_argument("Invalid element range guard");
    out<<"define i32 @"<<symbol<<"(ptr readonly %parameters) {\nentry:\n";
    Emitter e(out);
    const auto count=e.parameter(0),first=e.parameter(1),counter=e.base(int(guard.counter));
    e.require(e.value("icmp ne i64 "+count+", 0"),1);
    e.require(e.value("icmp ne i64 "+counter+", 0"),1);
    const auto remainder=e.value("urem i64 "+counter+", "+std::to_string(guard.step));
    e.require(e.value("icmp eq i64 "+remainder+", 0"),1);
    const auto iterations=e.value("udiv i64 "+counter+", "+std::to_string(guard.step));
    e.require(e.value("icmp ule i64 "+iterations+", "+std::to_string(guard.rows)),1);
    e.require(e.value("icmp uge i64 "+first+", "+std::to_string(guard.minimum_element)),2);
    e.require(e.value("icmp ule i64 "+e.add(first,count)+", "+iterations),1);
    const auto last=e.add(iterations,"-1"),selected_last=e.add(e.add(first,count),"-1");
    // Signed arithmetic in i64 preserves negative offsets without host UB.
    // An unsigned bound test rejects both a negative result and an overflowed VI.
    for (unsigned reg=1;reg<16;++reg) {
        const auto initial=e.base(int(reg)); e.bound(initial,65535);
        e.bound(e.add(initial,e.multiply(iterations,guard.steps[reg])),65535);
    }
    for (const auto& write:guard.integer_writes) {
        const auto initial=e.add(e.base(write.base),std::to_string(write.offset));
        e.bound(initial,65535);
        e.bound(e.add(initial,e.multiply(last,write.base>0 && write.base<16 ? guard.steps[write.base]:0)),65535);
    }
    const auto output_first=e.add(e.base(int(guard.output_base)),std::to_string(guard.output_first));
    const auto output_last=e.add(e.add(e.base(int(guard.output_base)),std::to_string(guard.output_last)),e.multiply(last,guard.output_stride));
    e.bound(output_first,guard.rows-1); e.bound(output_last,guard.rows-1);
    for (const auto& read:guard.reads) {
        const auto origin=e.add(e.base(read.base),std::to_string(read.offset));
        const auto begin=e.add(origin,e.multiply(read.selected ? first:"0",read.stride));
        const auto end=e.add(origin,e.multiply(read.selected ? selected_last:last,read.stride));
        e.bound(begin,guard.rows-1); e.bound(end,guard.rows-1);
        const auto& low=read.stride<0 ? end:begin;
        const auto& high=read.stride<0 ? begin:end;
        const auto before=e.value("icmp ult i64 "+high+", "+output_first);
        const auto after=e.value("icmp ugt i64 "+low+", "+output_last);
        e.require(e.value("or i1 "+before+", "+after),4);
    }
    out<<"  ret i32 0\n";
    for (unsigned code=1;code<=4;++code) out<<"fail"<<code<<":\n  ret i32 "<<code<<"\n";
    out<<"}\n";
    if (!out) throw std::runtime_error("Cannot emit element range guard");
}
}
