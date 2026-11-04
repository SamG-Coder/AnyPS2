#include <nt/packed_image.hpp>
#include <nt/elf.hpp>
#include <zlib.h>
#include <iostream>
#include <stdexcept>
#include <array>
using nt::Bytes;
void check(bool ok) { if (!ok) throw std::runtime_error("Packed executable regression"); }
void word(Bytes& b,unsigned value) { for (unsigned n=0;n<4;++n) b.push_back(static_cast<std::uint8_t>(value>>(8*n))); }
Bytes pack(const Bytes& decoded) {
    Bytes b{1,1}; word(b,static_cast<unsigned>(decoded.size())); b.resize(6+compressBound(static_cast<uLong>(decoded.size())));
    z_stream z{}; check(deflateInit2(&z,Z_DEFAULT_COMPRESSION,Z_DEFLATED,-15,8,Z_DEFAULT_STRATEGY)==Z_OK);
    z.next_in=const_cast<Bytef*>(decoded.data()); z.avail_in=static_cast<uInt>(decoded.size());
    z.next_out=b.data()+6; z.avail_out=static_cast<uInt>(b.size()-6);
    check(deflate(&z,Z_FINISH)==Z_STREAM_END); const auto size=z.total_out; deflateEnd(&z); b.resize(6+size); return b;
}
template<class F> void rejected(F f) { bool failed=false; try { f(); } catch(const std::exception&) { failed=true; } check(failed); }
int main(int argc,char** argv) {
    try {
        // Opaque empty metadata, three original synthetic segments including a
        // repeated subrange. No commercial instructions or compressed fixtures.
        Bytes raw{0,0,0,0}; word(raw,3); word(raw,0x2000);
        word(raw,0x2000); word(raw,8); word(raw,0x2402002a); word(raw,12);
        word(raw,0x2004); word(raw,4); word(raw,12);
        word(raw,0x3000); word(raw,4); word(raw,17);
        auto packed=pack(raw);
        if (argc==2) {
            nt::write_file(argv[1],packed);
            Bytes cross{0,0,0,0}; word(cross,2); word(cross,0x2000);
            word(cross,0x2000); word(cross,28);
            for (const auto instruction:{0x24083008U,0x24020029U,0xad020000U,0x0c000c00U,0U,0x2403007bU,12U})
                word(cross,instruction);
            word(cross,0x3000); word(cross,12);
            for (const auto value:{0x03e00008U,0x24420001U,0xdeadbeefU}) word(cross,value);
            nt::write_file(std::string(argv[1])+".cross",pack(cross));
            Bytes whole{0,0,0,0}; word(whole,3); word(whole,0x2000);
            word(whole,0x2000); word(whole,0x50);
            constexpr unsigned code[]{0x24083000,0x8d090000,0x0120f809,0,0x2403007b,12,0,0,
                0x2402002a,0x03e00008,0,0,0,0,0,0,0x2402004d,0x03e00008,0,0};
            for (const auto instruction:code) word(whole,instruction);
            word(whole,0x3000); word(whole,8); word(whole,0x2020); word(whole,0x4c000000);
            word(whole,0x3010); word(whole,8); word(whole,0x4c000000); word(whole,0x4c000000);
            const auto whole_packed=pack(whole);
            nt::write_file(std::string(argv[1])+".whole",whole_packed);
            nt::write_text_file(std::string(argv[1])+".layout","anyps2-packed-layout-v1 "+std::to_string(nt::fingerprint(whole_packed))+
                "\nee 0x2000 0x50\ndata 0x3000 8\nvu 0x3010 8\n");
        }
        const auto image=nt::package::unpack(packed);
        const auto bytes=nt::package::elf_image(image); const auto elf=nt::elf::parse(bytes);
        check(elf.entry==0x2000 && elf.segments.size()==2 && elf.segments[0].file_size==8);
        check(elf.inferred_permissions);
        check(nt::le32(bytes,elf.segments[0].offset)==0x2402002a && elf.segments[0].flags==7 && elf.segments[1].flags==6);
        using nt::package::Content;
        const std::vector<nt::package::Region> layout{{0x2000,8,Content::ee},{0x3000,4,Content::vu}};
        const auto classified=nt::elf::parse(nt::package::elf_image(image,layout));
        check(!classified.inferred_permissions && classified.code_sections.size()==1 && classified.vector_sections.size()==1);
        check(classified.code_sections.front().address==0x2000 && classified.code_sections.front().size==8);
        check(!nt::elf::executable_address(classified,0x3000));
        const auto declaration="anyps2-packed-layout-v1 "+std::to_string(nt::fingerprint(packed))+"\nee 0x2000 8\nvu 0x3000 4\n";
        const Bytes declaration_bytes(declaration.begin(),declaration.end());
        const auto parsed_layout=nt::package::parse_layout(declaration_bytes,nt::fingerprint(packed));
        check(parsed_layout.size()==2 && parsed_layout[1].content==Content::vu);
        rejected([&] { nt::package::parse_layout(declaration_bytes,nt::fingerprint(packed)^1); });
        for (const auto& bad_text:{std::string("anyps2-packed-layout-v1 1"),std::string("anyps2-packed-layout-v1 1 ee 0x2000"),
            std::string("anyps2-packed-layout-v1 1 invalid 0x2000 8"),std::string("anyps2-packed-layout-v1 1 ee 0xfffffffc 8")}) {
            const Bytes text(bad_text.begin(),bad_text.end());
            rejected([&] { nt::package::parse_layout(text,1); });
        }
        for (unsigned kind=0;kind<7;++kind) {
            auto invalid=layout;
            if (kind==0) invalid.pop_back();
            if (kind==1) invalid[0].size=4;
            if (kind==2) invalid[1].address=0x2004;
            if (kind==3) invalid[0].content=Content::data;
            if (kind==4) invalid[0].size=7;
            if (kind==5) invalid[1].size=8;
            if (kind==6) invalid.push_back({0x4000,4,Content::data});
            rejected([&] { nt::package::elf_image(image,invalid); });
        }
        auto bad_note=bytes;
        bad_note.back()=1;
        rejected([&] { nt::elf::parse(bad_note); });
        auto conflict=image; conflict.segments[1].data[0]^=1;
        rejected([&] { nt::package::elf_image(conflict); });
        auto bad=packed; bad.push_back(0); rejected([&] { nt::package::unpack(bad); });
        bad=packed; bad.pop_back(); rejected([&] { nt::package::unpack(bad); });
        bad=packed; bad[2]++; rejected([&] { nt::package::unpack(bad); });
        bad=packed; bad[5]=127; rejected([&] { nt::package::unpack(bad); });
        bad={1,1,0}; rejected([&] { nt::package::unpack(bad); });
        auto truncated=raw; truncated.pop_back(); rejected([&] { nt::package::unpack(pack(truncated)); });
        auto extra=raw; extra.push_back(0); rejected([&] { nt::package::unpack(pack(extra)); });
        auto missing=image; missing.entry=0x4000; rejected([&] { nt::package::elf_image(missing); });
        missing=image; missing.segments[0].address=0x1fffffc; rejected([&] { nt::package::elf_image(missing); });
        std::cout<<"Packed executable conversion passed\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
