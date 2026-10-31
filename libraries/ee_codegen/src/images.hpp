#pragma once
#include "llvm_writer.hpp"
#include <algorithm>
#include <set>
#include <stdexcept>
namespace nt::ee_codegen {
inline void validate_images(std::span<const ExecutableImage> images, std::span<const CodeRange> ranges,
                            std::span<const r5900::Instruction> instructions) {
    std::set<std::uint32_t> entries,ids;
    if (images.size()>32) throw std::invalid_argument("Too many native executable images");
    for (const auto& image:images) {
        if (!image.id || !image.entry || image.entry%4 || image.segments.empty() || image.segments.size()>64 ||
            !entries.insert(image.entry).second || !ids.insert(image.id).second)
            throw std::invalid_argument("Invalid or duplicate native executable image");
        bool entry_present=false;
        for (const auto& segment:image.segments) {
            if (segment.bytes.empty() || std::uint64_t(segment.address)+segment.bytes.size()>0x2000000U)
                throw std::invalid_argument("Native image exceeds RAM bounds");
            entry_present|=image.entry>=segment.address && std::uint64_t(image.entry)+4<=std::uint64_t(segment.address)+segment.bytes.size();
        }
        if (!entry_present) throw std::invalid_argument("Native image entry is not file-backed");
    }
    for (const auto& range:ranges) if (range.image) {
        const auto image=std::find_if(images.begin(),images.end(),[&](const auto& i) { return i.id==range.image; });
        if (image==images.end()) throw std::invalid_argument("Executable range has no image owner");
        if (!std::any_of(image->segments.begin(),image->segments.end(),[&](const auto& s) {
            return range.address>=s.address && std::uint64_t(range.address)+range.size<=std::uint64_t(s.address)+s.bytes.size();
        })) throw std::invalid_argument("Executable range is not file-backed by its image");
    }
    for (const auto& image:images) {
        bool compiled_entry=false;
        for (const auto& i:instructions) {
            const auto owner=std::find_if(ranges.begin(),ranges.end(),[&](const auto& r) {
                return r.image==image.id && i.pc>=r.address && std::uint64_t(i.pc)+4<=std::uint64_t(r.address)+r.size;
            });
            if (owner==ranges.end()) continue;
            compiled_entry|=i.pc==image.entry;
            for (const auto& segment:image.segments) if (i.pc>=segment.address && std::uint64_t(i.pc)+4<=std::uint64_t(segment.address)+segment.bytes.size()) {
                const auto offset=i.pc-segment.address;
                std::uint32_t word=0;
                for (unsigned b=0;b<4;++b) word|=std::uint32_t(segment.bytes[offset+b])<<(b*8);
                if (word!=i.word) throw std::invalid_argument("Compiled image instruction differs from expected bytes");
            }
        }
        if (!compiled_entry) throw std::invalid_argument("Native image entry has no owned compiled code");
        for (std::size_t a=0;a<image.segments.size();++a) for (std::size_t b=0;b<a;++b) {
            const auto& x=image.segments[a]; const auto& y=image.segments[b];
            if (x.address<std::uint64_t(y.address)+y.bytes.size() && y.address<std::uint64_t(x.address)+x.bytes.size())
                throw std::invalid_argument("Native image segments overlap");
        }
    }

}
inline void emit_images(Writer& w,std::span<const ExecutableImage> images) {
    // Constant build artifacts are compared once at the explicit handoff, never
    // fetched/decoded by execution. Escaped strings avoid enormous i8 lists.
    constexpr char hex[]="0123456789ABCDEF";
    for (const auto& image:images) for (std::size_t n=0;n<image.segments.size();++n) {
        const auto& segment=image.segments[n];
        w.out<<"@nt_image_"<<image.id<<"_"<<n<<" = private constant ["<<segment.bytes.size()<<" x i8] c\"";
        for (auto byte:segment.bytes) w.out<<'\\'<<hex[byte>>4]<<hex[byte&15];
        w.out<<"\"\n";
    }
    w.out<<"define i32 @nt_ee_generated_publish_image(ptr %memory, i32 %target) {\nentry:\n";
    if (images.empty()) { w.out<<"  ret i32 0\n}\n"; return; }
    w.out<<"  switch i32 %target, label %missing [\n";
    for (const auto& image:images) w.out<<"    i32 "<<image.entry<<", label %image"<<image.id<<"\n";
    w.out<<"  ]\nmissing:\n  ret i32 2\n";
    for (const auto& image:images) {
        w.out<<"image"<<image.id<<":\n";
        const auto data=w.load("ptr",offsetof(ee::Memory,data),"%memory");
        const auto size=w.load("i64",offsetof(ee::Memory,size),"%memory");
        std::uint64_t extent=0;
        for (const auto& segment:image.segments) extent=std::max(extent,std::uint64_t(segment.address)+segment.bytes.size());
        const auto bad=w.binary("or","i1",w.value("icmp eq ptr "+data+", null"),w.value("icmp ult i64 "+size+", "+std::to_string(extent)));
        const auto suffix=std::to_string(image.id);
        w.out<<"  br i1 "<<bad<<", label %invalid"<<suffix<<", label %compare"<<suffix<<"\ncompare"<<suffix<<":\n";
        std::string matches="true";
        for (std::size_t n=0;n<image.segments.size();++n) {
            const auto& segment=image.segments[n];
            const auto equal=w.value("call i32 @memcmp(ptr "+w.pointer(segment.address,data)+", ptr @nt_image_"+
                suffix+"_"+std::to_string(n)+", i64 "+std::to_string(segment.bytes.size())+")");
            matches=w.binary("and","i1",matches,w.value("icmp eq i32 "+equal+", 0"));
        }
        w.out<<"  br i1 "<<matches<<", label %publish"<<suffix<<", label %invalid"<<suffix<<"\ninvalid"<<suffix<<":\n";
        w.store("i32","%target",offsetof(ee::Memory,fault_address),"%memory");
        w.out<<"  ret i32 14\npublish"<<suffix<<":\n";
        w.store("i32",suffix,offsetof(ee::Memory,executable_image),"%memory");
        w.out<<"  ret i32 0\n";
    }
    w.out<<"}\n";
}
}
