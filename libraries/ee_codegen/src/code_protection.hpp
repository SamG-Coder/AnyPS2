#pragma once
#include "llvm_writer.hpp"
#include <algorithm>
#include <map>

namespace nt::ee_codegen {
// Emit native comparisons once for a sparse code layout, rather than repeating
// every extent at every store. Each function owns at most 32 range decisions.
inline std::string emit_protection_tree(Writer& w,std::span<const CodeRange> ranges,std::size_t& next) {
    const auto name="nt_ee_protection_"+std::to_string(next++);
    const auto middle=ranges.size()/2;
    std::string left,right;
    if (ranges.size()>32) {
        left=emit_protection_tree(w,ranges.first(middle),next);
        right=emit_protection_tree(w,ranges.subspan(middle),next);
    }
    w.out<<"define internal i1 @"<<name<<"(i64 %begin, i64 %end) noinline {\nentry:\n";
    if (!left.empty()) {
        const auto before=w.value("icmp ult i64 %begin, "+std::to_string(ranges[middle].address));
        w.out<<"  br i1 "<<before<<", label %left, label %right\nleft:\n";
        const auto hit=w.value("call i1 @"+left+"(i64 %begin, i64 %end)");
        w.out<<"  br i1 "<<hit<<", label %hit, label %check_right\ncheck_right:\n";
        const auto after=w.value("icmp ugt i64 %end, "+std::to_string(ranges[middle].address));
        w.out<<"  br i1 "<<after<<", label %right, label %miss\nright:\n";
        const auto result=w.value("call i1 @"+right+"(i64 %begin, i64 %end)");
        w.out<<"  ret i1 "<<result<<"\n";
    } else {
        const auto label=[](std::size_t first,std::size_t last) {
            return first==last ? std::string("%miss") : "%range"+std::to_string((first+last)/2);
        };
        w.out<<"  br label "<<label(0,ranges.size())<<"\n";
        const auto emit=[&](auto&& self,std::size_t first,std::size_t last)->void {
            if (first==last) return;
            const auto index=(first+last)/2;
            const auto& range=ranges[index];
            w.out<<"range"<<index<<":\n";
            const auto beyond=w.value("icmp uge i64 %begin, "+std::to_string(std::uint64_t(range.address)+range.size));
            w.out<<"  br i1 "<<beyond<<", label "<<label(index+1,last)<<", label %before"<<index<<"\nbefore"<<index<<":\n";
            const auto before=w.value("icmp ule i64 %end, "+std::to_string(range.address));
            w.out<<"  br i1 "<<before<<", label "<<label(first,index)<<", label %hit\n";
            self(self,first,index); self(self,index+1,last);
        };
        emit(emit,0,ranges.size());
    }
    w.out<<"hit:\n  ret i1 true\nmiss:\n  ret i1 false\n}\n";
    return name;
}
inline void emit_code_protection(Writer& w,std::span<const CodeRange> ranges) {
    std::map<std::uint32_t,std::vector<CodeRange>> owners;
    for (const auto& range:ranges) owners[range.image].push_back(range);
    std::map<std::uint32_t,std::string> roots;
    std::size_t next=0;
    for (const auto& [image,code]:owners) roots[image]=emit_protection_tree(w,code,next);
    w.out<<"define i1 @nt_ee_check_code_range(i64 %begin, i64 %end, i32 %image) noinline {\nentry:\n";
    const auto empty=w.value("icmp uge i64 %begin, %end");
    w.out<<"  br i1 "<<empty<<", label %miss, label %common\ncommon:\n";
    if (const auto common=roots.find(0);common!=roots.end()) {
        const auto hit=w.value("call i1 @"+common->second+"(i64 %begin, i64 %end)");
        w.out<<"  br i1 "<<hit<<", label %hit, label %select_image\n";
    } else w.out<<"  br label %select_image\n";
    w.out<<"select_image:\n  switch i32 %image, label %miss [\n";
    for (const auto& [image,root]:roots) if (image) w.out<<"    i32 "<<image<<", label %image"<<image<<"\n";
    w.out<<"  ]\n";
    for (const auto& [image,root]:roots) if (image) {
        w.out<<"image"<<image<<":\n";
        const auto result=w.value("call i1 @"+root+"(i64 %begin, i64 %end)");
        w.out<<"  ret i1 "<<result<<"\n";
    }
    w.out<<"hit:\n  ret i1 true\nmiss:\n  ret i1 false\n}\n";
}
}
