#pragma once
#include "llvm_writer.hpp"
#include <vector>

namespace nt::ee_codegen {
// Shared constant-address routing for native entry dispatch and membership.
// Emitted branches contain no runtime address table.
template<class Last,class Leaf>
void emit_address_routes(Writer& w,std::size_t count,const std::string& pc,const std::string& prefix,Last last,Leaf leaf) {
    const auto route=[&](auto&& self,std::size_t first,std::size_t end)->void {
        w.out<<prefix<<first<<"_"<<end<<":\n";
        if (end-first==1) { leaf(first); return; }
        const auto mid=first+(end-first)/2;
        const auto left=w.value("icmp ule i32 "+pc+", "+std::to_string(last(mid-1)));
        w.out<<"  br i1 "<<left<<", label %"<<prefix<<first<<"_"<<mid<<", label %"<<prefix<<mid<<"_"<<end<<"\n";
        self(self,first,mid); self(self,mid,end);
    };
    route(route,0,count);
}
inline void emit_contains(Writer& w,std::span<const r5900::Instruction> instructions) {
    std::vector<std::pair<std::uint32_t,std::uint32_t>> ranges;
    for (const auto& i:instructions) {
        if (!ranges.empty() && std::uint64_t(ranges.back().second)+4==i.pc) ranges.back().second=i.pc;
        else ranges.emplace_back(i.pc,i.pc);
    }
    w.out<<"define i1 @nt_ee_generated_contains(i32 %address) {\nentry:\n";
    const auto aligned=w.value("icmp eq i32 "+w.binary("and","i32","%address","3")+", 0");
    w.out<<"  br i1 "<<aligned<<", label %contains_route0_"<<ranges.size()<<", label %missing\n";
    emit_address_routes(w,ranges.size(),"%address","contains_route",[&](auto index) { return ranges[index].second; },[&](auto index) {
        const auto [first,last]=ranges[index];
        const auto offset=w.binary("sub","i32","%address",std::to_string(first));
        const auto present=w.value("icmp ule i32 "+offset+", "+std::to_string(last-first));
        w.out<<"  ret i1 "<<present<<"\n";
    });
    w.out<<"missing:\n  ret i1 false\n}\n";
}
inline void emit_dispatcher(Writer& w,const std::vector<std::span<const r5900::Instruction>>& parts) {
    w.out<<"define i32 @nt_ee_generated_run(ptr %state, ptr %memory, i64 %budget) {\nentry:\n"
         <<"  %remaining = alloca i64\n  store i64 %budget, ptr %remaining\n  br label %dispatch\ndispatch:\n";
    const auto pc=w.load("i32",offsetof(runtime::State,pc));
    const auto before=w.load("i64",offsetof(runtime::State,executed));
    const auto budget=w.value("load i64, ptr %remaining");
    w.out<<"  br label %route0_"<<parts.size()<<"\n";
    emit_address_routes(w,parts.size(),pc,"route",[&](auto index) { return parts[index].back().pc; },[&](auto first) {
        const auto low=w.value("icmp uge i32 "+pc+", "+std::to_string(parts[first].front().pc));
        const auto high=w.value("icmp ule i32 "+pc+", "+std::to_string(parts[first].back().pc));
        const auto inside=w.binary("and","i1",low,high);
        w.out<<"  br i1 "<<inside<<", label %call"<<first<<", label %unmapped\n";
    });
    for (std::size_t index=0;index<parts.size();++index) {
        w.out<<"call"<<index<<":\n  %result"<<index<<" = call i32 @nt_ee_part_"<<parts[index].front().pc
             <<"(ptr %state, ptr %memory, i64 "<<budget<<")\n  br label %returned\n";
    }
    w.out<<"returned:\n  %stop = phi i32 ";
    for (std::size_t index=0;index<parts.size();++index)
        w.out<<(index ? ", " : "")<<"[ %result"<<index<<", %call"<<index<<" ]";
    w.out<<"\n  %first = phi i32 ";
    for (std::size_t index=0;index<parts.size();++index)
        w.out<<(index ? ", " : "")<<"[ "<<parts[index].front().pc<<", %call"<<index<<" ]";
    w.out<<"\n  %last = phi i32 ";
    for (std::size_t index=0;index<parts.size();++index)
        w.out<<(index ? ", " : "")<<"[ "<<parts[index].back().pc<<", %call"<<index<<" ]";
    w.out<<"\n";
    const auto transfer=w.value("icmp eq i32 %stop, 2");
    w.out<<"  br i1 "<<transfer<<", label %transfer, label %done\ntransfer:\n";
    const auto next_pc=w.load("i32",offsetof(runtime::State,pc));
    const auto low=w.value("icmp uge i32 "+next_pc+", %first");
    const auto high=w.value("icmp ule i32 "+next_pc+", %last");
    const auto same=w.binary("and","i1",low,high);
    w.out<<"  br i1 "<<same<<", label %unmapped, label %continue\ncontinue:\n";
    const auto after=w.load("i64",offsetof(runtime::State,executed));
    const auto retired=w.binary("sub","i64",after,before);
    const auto left=w.binary("sub","i64",budget,retired);
    w.out<<"  store i64 "<<left<<", ptr %remaining\n";
    const auto empty=w.value("icmp eq i64 "+left+", 0");
    w.out<<"  br i1 "<<empty<<", label %budget_stop, label %dispatch\ndone:\n  ret i32 %stop\n"
         <<"unmapped:\n  ret i32 2\nbudget_stop:\n  ret i32 0\n}\n";
}
}
