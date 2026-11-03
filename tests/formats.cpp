#include "disc_fixture.hpp"
#include "reboot_fixture.hpp"
#include "vector_section_fixture.hpp"
#include <nt/reboot_image.hpp>
#include <nt/iop_module.hpp>
#include <nt/iop_libraries.hpp>
#include <nt/elf.hpp>
#include <nt/iso9660.hpp>
#include <chrono>
#include <iostream>
#include <stdexcept>

namespace {
using fixture::put32; using fixture::both32; using fixture::record;
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class F> void rejects(F function) {
    bool rejected = false;
    try { function(); } catch (const std::exception&) { rejected = true; }
    check(rejected, "Malformed input was accepted");
}
nt::Bytes make_elf() {
    nt::Bytes b(260, 0);
    const std::uint8_t magic[] = {0x7f, 'E', 'L', 'F', 1, 1, 1};
    std::copy(std::begin(magic), std::end(magic), b.begin());
    b[16] = 2; b[18] = 8; b[20] = 1; b[40] = 52; b[42] = 32; b[44] = 1;
    put32(b, 24, 0x100000); put32(b, 28, 52);
    put32(b, 52, 1); put32(b, 56, 256); put32(b, 60, 0x100000);
    put32(b, 68, 4); put32(b, 72, 8); put32(b, 76, 5); put32(b, 80, 4);
    return b;
}
void test_elf() {
    const auto good = make_elf();
    const auto parsed = nt::elf::parse(good);
    check(parsed.entry == 0x100000 && parsed.segments.size() == 1, "ELF metadata differs");
    for (std::size_t length = 0; length < good.size(); ++length)
        rejects([&] { nt::elf::parse(nt::ByteView(good).first(length)); });
    for (const auto offset : {18U, 42U, 44U}) {
        auto bad = good; bad[offset] = 0;
        rejects([&] { nt::elf::parse(bad); });
    }
    auto bad = good; put32(bad, 24, 0x100004);
    rejects([&] { nt::elf::parse(bad); });
    bad = good; put32(bad, 72, 3);
    rejects([&] { nt::elf::parse(bad); });
    bad = good; put32(bad, 28, 0xfffffff0);
    rejects([&] { nt::elf::parse(bad); });
    auto lifecycle=good; lifecycle.resize(0x200,0);
    put32(lifecycle,32,0x80); lifecycle[46]=40; lifecycle[48]=2;
    put32(lifecycle,68,16); put32(lifecycle,72,16);
    // Original allocated INIT_ARRAY with a pointer into executable file data.
    put32(lifecycle,0xac,14); put32(lifecycle,0xb0,2); put32(lifecycle,0xb4,0x100004);
    put32(lifecycle,0xb8,0x104); put32(lifecycle,0xbc,4); put32(lifecycle,0xcc,4);
    put32(lifecycle,0x104,0x100000);
    {
        const auto sections=nt::elf::parse(lifecycle);
        check(nt::elf::executable_address(sections,0x100000) &&
              !nt::elf::executable_address(sections,0x100004) &&
              !nt::elf::executable_address(sections,0x100002,4),
              "Allocated lifecycle data inside executable LOAD was treated as code");
        auto invalid=lifecycle; put32(invalid,24,0x100004);
        rejects([&] { nt::elf::parse(invalid); });
    }
    for (const auto type:{14U,15U,16U}) {
        put32(lifecycle,0xac,type);
        check(nt::elf::parse(lifecycle).lifecycle_entries==std::vector<std::uint32_t>{0x100000},"Lifecycle root lost");
    }
    for (const auto [offset,value]:std::initializer_list<std::pair<unsigned,unsigned>>{
        {0xb8,0x108},{0xbc,3},{0xcc,8},{0x104,0x100001},{0x104,0x100010},{32,0xfffffff0}}) {
        auto invalid=lifecycle; put32(invalid,offset,value);
        rejects([&] { nt::elf::parse(invalid); });
    }
    // Named legacy constructor/destructor tables with sentinel and count forms.
    lifecycle[48]=3; lifecycle[50]=2;
    put32(lifecycle,0xa8,1); put32(lifecycle,0xac,1); put32(lifecycle,0xbc,12); put32(lifecycle,0xcc,0);
    put32(lifecycle,0xd4,3); put32(lifecycle,0xe0,0x180); put32(lifecycle,0xe4,8);
    const std::string name("\0.ctors\0",8); std::copy(name.begin(),name.end(),lifecycle.begin()+0x180);
    put32(lifecycle,0x104,0xffffffff); put32(lifecycle,0x108,0x80100000); put32(lifecycle,0x10c,0);
    check(nt::elf::parse(lifecycle).lifecycle_entries==std::vector<std::uint32_t>{0x100000},"Legacy aliased constructor root lost");
    lifecycle[0x182]='d'; put32(lifecycle,0x104,1);
    check(nt::elf::parse(lifecycle).lifecycle_entries.size()==1,"Legacy destructor count form lost");
    auto invalid=lifecycle; put32(invalid,0xe4,2); rejects([&] { nt::elf::parse(invalid); });
}
void test_vector_section() {
    const auto bytes=fixture::vector_container_elf();
    const auto parsed=nt::elf::parse(bytes);
    check(parsed.vector_sections.size()==1 && parsed.vector_sections[0].address==0x1060 &&
          parsed.vector_sections[0].size==16,"ELF VU container metadata was lost");
    check(nt::elf::executable_address(parsed,0x1000) && !nt::elf::executable_address(parsed,0x1060) &&
          !nt::elf::executable_address(parsed,0x105c,8),"VU container was exposed as EE code");
    for (const auto [at,value]:std::initializer_list<std::pair<unsigned,unsigned>>{
            {24,0x1060},{0x234,0x2000},{0x238,0x164},{0x23c,0x100},{0x228,19}}) {
        auto bad=bytes; put32(bad,at,value);
        rejects([&] { nt::elf::parse(bad); });
    }
    auto bad=bytes; std::fill(bad.begin()+0x2a0,bad.begin()+0x2b3,'x');
    rejects([&] { nt::elf::parse(bad); });
    auto overlays=make_elf(); overlays.resize(0x400,0);
    put32(overlays,68,16); put32(overlays,72,16);
    put32(overlays,32,0x200); overlays[46]=40; overlays[48]=4; overlays[50]=1;
    constexpr char names[]="\0.shstrtab\0.DVP.overlay.synthetic";
    std::copy(std::begin(names),std::end(names),overlays.begin()+0x300);
    put32(overlays,0x228,1); put32(overlays,0x22c,3); put32(overlays,0x238,0x300); put32(overlays,0x23c,sizeof(names));
    put32(overlays,0x254,0x7ffff420); put32(overlays,0x260,0x380); put32(overlays,0x264,12);
    put32(overlays,0x268,1); put32(overlays,0x274,12);
    put32(overlays,0x278,11); put32(overlays,0x27c,0x7ffff421); put32(overlays,0x28c,8);
    put32(overlays,0x380,11); put32(overlays,0x384,0x100008); put32(overlays,0x388,8);
    const auto recovered=nt::elf::parse(overlays).vector_overlays;
    check(recovered.size()==1 && recovered[0].source_address==0x100008 && recovered[0].destination==8 &&
        recovered[0].size==8 && recovered[0].file_offset==0x108,"DVP metadata did not resolve load-image bytes");
    for (const auto [at,value]:std::initializer_list<std::pair<unsigned,unsigned>>{
            {0x264,11},{0x268,4},{0x380,999},{0x384,0x100010},{0x388,16384},{0x28c,7}}) {
        auto invalid=overlays; put32(invalid,at,value);
        rejects([&] { nt::elf::parse(invalid); });
    }
}
void test_function_symbols() {
    auto image=make_elf(); image.resize(0x340,0);
    put32(image,68,0x40); put32(image,72,0x80); put32(image,76,7);
    put32(image,32,0x180); image[46]=40; image[48]=5;
    const auto section=[&](unsigned index,unsigned type,unsigned flags,unsigned address,unsigned offset,unsigned size) {
        const auto at=0x180+index*40;
        put32(image,at+4,type); put32(image,at+8,flags); put32(image,at+12,address);
        put32(image,at+16,offset); put32(image,at+20,size);
    };
    section(1,1,6,0x100000,0x100,0x20);
    section(2,1,3,0x100020,0x120,0x20);
    section(3,3,0,0,0x300,8);
    section(4,2,0,0,0x280,64);
    put32(image,0x238,3); put32(image,0x244,16);
    const std::string names("\0entry\0",7); std::copy(names.begin(),names.end(),image.begin()+0x300);
    const auto symbol=[&](unsigned index,unsigned address,unsigned size,unsigned type,unsigned section) {
        const auto at=0x280+index*16;
        put32(image,at,1); put32(image,at+4,address); put32(image,at+8,size);
        image[at+12]=static_cast<std::uint8_t>(type); image[at+14]=static_cast<std::uint8_t>(section);
    };
    symbol(1,0x100008,8,2,1); // A local function, not reached from the ELF entry.
    symbol(2,0x100010,4,1,1); // An object in .text is not a function.
    symbol(3,0x100020,4,2,2); // The data section wins over a function annotation.
    check(nt::elf::parse(image).function_entries==std::vector<std::uint32_t>{0x100008},"ELF symbol types or section permissions lost");
    symbol(2,0x100008,0,0x22,1); // Weak alias with unspecified size.
    check(nt::elf::parse(image).function_entries==std::vector<std::uint32_t>{0x100008},"Duplicate weak function roots were not coalesced");
    symbol(2,0,0,0x22,0); // Undefined weak function.
    for (const auto [at,value]:std::initializer_list<std::pair<unsigned,unsigned>>{
            {0x294,0x100009},{0x298,0x40},{0x244,8},{0x238,5},{0x290,9},{0x230,0xfffffff0}}) {
        auto invalid=image; put32(invalid,at,value); rejects([&] { nt::elf::parse(invalid); });
    }
    image[0x29e]=5;
    rejects([&] { nt::elf::parse(image); });
}
void test_reboot_image() {
    const auto image=fixture::reboot_image();
    const auto files=nt::disc::parse_reboot_image(image);
    check(files.size()==4 && files[3].name=="EXAMPLE" && files[3].offset==96 &&
          files[3].size==19 && files[3].info_offset==80 && files[3].info_size==4,
          "Reboot image extents or metadata differ");
    for (std::size_t length=0;length<image.size();++length)
        rejects([&] { nt::disc::parse_reboot_image(nt::ByteView(image).first(length)); });
    for (const auto offset : {12U,28U,44U,60U}) {
        auto bad=image; put32(bad,offset,0xffffffff);
        rejects([&] { nt::disc::parse_reboot_image(bad); });
    }
    for (const auto offset : {5U,58U,64U,74U,82U}) {
        auto bad=image; bad[offset]=1;
        rejects([&] { nt::disc::parse_reboot_image(bad); });
    }
    auto duplicate=image; std::copy_n(image.begin(),10,duplicate.begin()+48);
    rejects([&] { nt::disc::parse_reboot_image(duplicate); });
}
nt::Bytes make_irx() {
    auto file=make_elf();
    file.resize(352,0);
    file[16]=0x80; file[17]=0xff;
    put32(file,24,0); put32(file,32,84);
    file[46]=40; file[48]=2;
    put32(file,60,0); put32(file,68,32); put32(file,72,64); put32(file,80,16);
    // A null section followed by a version-one REL table.
    put32(file,128,9); put32(file,140,288); put32(file,144,32); put32(file,160,8);
    put32(file,256,0x20); put32(file,260,0x08000008);
    put32(file,264,0x3c080001); put32(file,268,0x2508fff0);
    for (unsigned i=0;i<4;++i) {
        put32(file,288+i*8,i*4);
        // IRX uses the image base rather than ELF symbol values.
        put32(file,292+i*8,0x100 | (i==0 ? 2 : i==1 ? 4 : i==2 ? 5 : 6));
    }
    return file;
}
void test_iop_module() {
    const auto file=make_irx();
    for (const auto base : {0x10000U,0x18000U,0x1fff0U}) {
        const auto module=nt::elf::load_iop_module(file,base);
        const auto relocated=base+0xfff0;
        check(module.base==base && module.entry==base && module.memory.size()==64,
              "IRX allocation or entry differs");
        check(nt::le32(module.memory,0)==base+0x20 && nt::le32(module.memory,4)==0x08000008+(base>>2) &&
              nt::le32(module.memory,8)==(0x3c080000|((relocated+0x8000)>>16)) &&
              nt::le32(module.memory,12)==(0x25080000|(relocated&0xffff)),
              "IRX word, jump or signed HI16/LO16 carry relocation differs");
        check(std::all_of(module.memory.begin()+32,module.memory.end(),[](auto c) { return c==0; }),
              "IRX BSS was not zero initialized");
    }
    check(file==make_irx(),"IRX loading modified caller input");
    for (std::size_t length=0;length<320;++length)
        rejects([&] { nt::elf::load_iop_module(nt::ByteView(file).first(length),0x10000); });
    for (const auto base : {1U,0x200000U,0x1ffff0U})
        rejects([&] { nt::elf::load_iop_module(file,base); });
    for (const auto offset : {24U,60U,68U,72U,140U,144U,160U,288U}) {
        auto bad=file; put32(bad,offset,0xffffffff);
        rejects([&] { nt::elf::load_iop_module(bad,0x10000); });
    }
    for (const auto kind : {1U,3U,7U,255U}) {
        auto bad=file; put32(bad,292,kind);
        rejects([&] { nt::elf::load_iop_module(bad,0x10000); });
    }
    auto bad=file; put32(bad,316,0); // Unmatched high half.
    rejects([&] { nt::elf::load_iop_module(bad,0x10000); });
    bad=file; put32(bad,308,0); // Unmatched low half.
    rejects([&] { nt::elf::load_iop_module(bad,0x10000); });
    bad=file; bad[16]=0x81;
    rejects([&] { nt::elf::load_iop_module(bad,0x10000); });

    auto described=file; described.resize(448,0);
    std::copy_n(file.begin()+52,32,described.begin()+384);
    put32(described,28,352); described[44]=2;
    put32(described,352,0x70000080); put32(described,356,416); put32(described,368,32);
    put32(described,424,0x7ff0); put32(described,428,16);
    put32(described,432,16); put32(described,436,32);
    described[440]=2; described[441]=1;
    const std::string name="test"; std::copy(name.begin(),name.end(),described.begin()+442);
    const auto module=nt::elf::load_iop_module(described,0x10000);
    check(module.info && module.info->name==name && module.info->version==0x102 &&
          module.info->global_pointer==0x17ff0 && module.info->text_size==16 &&
          module.info->data_size==16 && module.info->bss_size==32,"IRX module metadata differs");
    auto callback_image=described;
    put32(callback_image,256,8);
    const auto callback_module=nt::elf::load_iop_module(callback_image,0x18000);
    check(callback_module.code_addresses==std::vector<std::uint32_t>{0x18008},
          "IRX code-address relocation was not retained for AOT discovery");
    check(module.code_addresses.empty(),"Data relocation was classified as executable");
    for (const auto offset : {368U,424U,428U,432U,436U}) {
        bad=described; put32(bad,offset,0xffffffff);
        rejects([&] { nt::elf::load_iop_module(bad,0x10000); });
    }
    bad=described; std::fill(bad.begin()+442,bad.end(),'x');
    rejects([&] { nt::elf::load_iop_module(bad,0x10000); });
}
void test_iop_imports() {
    auto file=make_irx(); file.resize(640,0);
    std::copy_n(file.begin()+256,32,file.begin()+512);
    put32(file,56,512); put32(file,68,128); put32(file,72,160);
    const auto table=[&](unsigned at,const std::string& name,unsigned ordinal) {
        put32(file,at,0x41e00000); file[at+8]=2; file[at+9]=1;
        std::copy(name.begin(),name.end(),file.begin()+at+12);
        put32(file,at+20,0x03e00008); put32(file,at+24,0x24000000|ordinal);
    };
    table(544,"example1",4); table(592,"second",7);
    const auto original=nt::elf::load_iop_module(file,0x10000);
    const auto imports=nt::elf::iop_imports(original);
    check(imports.size()==2 && imports[0].library=="example1" && imports[0].version==0x102 &&
          imports[0].ordinal==4 && imports[0].offset==52 && imports[1].library=="second" && imports[1].ordinal==7,
          "IRX import discovery or ordinals differ");
    auto linked=original;
    nt::elf::bind_iop_imports(linked,[](const nt::elf::IopImport& import) -> std::optional<std::uint32_t> {
        return import.library=="example1" ? 0x20000 : 0x30000;
    });
    check(nt::le32(linked.memory,52)==0x08008000 && nt::le32(linked.memory,100)==0x0800c000 &&
          nt::le32(linked.memory,56)==0x24000004 && nt::le32(linked.memory,104)==0x24000007,
          "IRX binding changed delay slots or jump targets");
    for (const auto target : {0U,3U,0x200000U}) {
        auto failed=original;
        rejects([&] { nt::elf::bind_iop_imports(failed,[&](const auto& import) -> std::optional<std::uint32_t> {
            return import.library=="example1" ? 0x20000 : target;
        }); });
        check(failed.memory==original.memory,"Failed import binding partially patched module");
    }
    auto missing=original;
    rejects([&] { nt::elf::bind_iop_imports(missing,[](const auto& import) -> std::optional<std::uint32_t> {
        if (import.library=="example1") return 0x20000;
        return {};
    }); });
    check(missing.memory==original.memory,"Unresolved import partially patched module");
    for (const auto offset : {36U,40U,52U,56U,60U,108U}) {
        auto bad=original; put32(bad.memory,offset,0xffffffff);
        rejects([&] { nt::elf::iop_imports(bad); });
    }
    auto truncated=original; truncated.initialized_size=104;
    rejects([&] { nt::elf::iop_imports(truncated); });

    // Load providers through the real relocator, explicitly register their
    // exports, then bind the loaded consumer without a synthetic resolver.
    const auto provider=[&](const std::string& name,unsigned base) {
        auto image=file;
        std::fill(image.begin()+544,image.end(),0);
        put32(image,544,0x41c00000); image[552]=2; image[553]=1;
        std::copy(name.begin(),name.end(),image.begin()+556);
        for (unsigned i=0;i<8;++i) {
            put32(image,564+i*4,16);
            put32(image,320+i*8,52+i*4); put32(image,324+i*8,2);
        }
        put32(image,144,96);
        return nt::elf::load_iop_module(image,base);
    };
    const auto first=provider("example1",0x20000), second=provider("second",0x30000);
    nt::iop::Libraries libraries;
    check(!libraries.resolve(imports[0]),"Unregistered exports became visible");
    libraries.register_exports(first,first.base+32);
    auto pending=original;
    rejects([&] { libraries.bind(pending); });
    check(pending.memory==original.memory && libraries.size()==1,"Missing provider partially bound consumer");
    rejects([&] { libraries.register_exports(first,first.base+32); });
    check(libraries.size()==1,"Duplicate registration changed registry");
    for (const auto offset : {32U,36U,40U,52U,84U}) {
        auto bad=second; put32(bad.memory,offset,0xffffffff);
        rejects([&] { libraries.register_exports(bad,bad.base+32); });
        check(libraries.size()==1,"Invalid export table changed registry");
    }
    auto no_terminator=second; no_terminator.initialized_size=84;
    rejects([&] { libraries.register_exports(no_terminator,no_terminator.base+32); });
    libraries.register_exports(second,second.base+32);
    libraries.bind(pending);
    check(nt::le32(pending.memory,52)==0x08008004 && nt::le32(pending.memory,100)==0x0800c004 &&
          nt::le32(pending.memory,56)==0x24000004,"Registered guest exports did not bind correctly");
    auto incompatible=imports[0]; incompatible.version=0x103;
    check(!libraries.resolve(incompatible),"Unsupported ABI version matched");
    incompatible=imports[0]; incompatible.ordinal=8;
    check(!libraries.resolve(incompatible),"Out-of-range export ordinal matched");
    libraries.reset();
    check(libraries.size()==0 && !libraries.resolve(imports[0]),"Reset retained exported functions");
}
void test_disc(const std::filesystem::path& path) {
    nt::Bytes iso(24 * 2048, 0);
    const auto pvd = 16 * 2048;
    iso[pvd] = 1;
    const std::string signature = "CD001";
    std::copy(signature.begin(), signature.end(), iso.begin() + pvd + 1);
    iso[pvd + 6] = 1; iso[pvd + 129] = 8; iso[pvd + 130] = 8;
    record(iso, pvd + 156, std::string(1, '\0'), 20, 2048, true);
    const std::string cnf = "BOOT2 = cdrom0:\\TEST.ELF;1\r\n";
    auto at = 20 * 2048;
    at += static_cast<int>(record(iso, at, "SYSTEM.CNF;1", 21, static_cast<std::uint32_t>(cnf.size()), false));
    const auto elf = make_elf();
    record(iso, at, "TEST.ELF;1", 22, static_cast<std::uint32_t>(elf.size()), false);
    std::copy(cnf.begin(), cnf.end(), iso.begin() + 21 * 2048);
    std::copy(elf.begin(), elf.end(), iso.begin() + 22 * 2048);
    nt::write_file(path, iso);
    {
        nt::disc::Iso9660 disc(path);
        const auto boot = nt::disc::boot_path(disc.read(disc.find("system.cnf")));
        check(boot == "TEST.ELF;1", "BOOT2 parsing failed");
        check(disc.read(disc.find(boot)) == elf, "ISO extraction changed bytes");
        rejects([&] { disc.find("../TEST.ELF"); });
        rejects([&] { disc.find("MISSING.ELF"); });
        rejects([&] { disc.read({"outside", 1000, 4, false}); });
    }
    iso[pvd + 156 + 6] ^= 1;
    nt::write_file(path, iso);
    rejects([&] { nt::disc::Iso9660 disc(path); });
    nt::Bytes empty;
    rejects([&] { nt::disc::boot_path(empty); });
}
}
int main() {
    const auto path = std::filesystem::temp_directory_path() /
        ("anyps2-format-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".iso");
    try {
        test_elf();
        test_function_symbols();
        test_vector_section();
        test_reboot_image();
        test_iop_module();
        test_iop_imports();
        test_disc(path);
        std::filesystem::remove(path);
        std::cout << "ELF, ISO and reboot archive positive/negative cases passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
