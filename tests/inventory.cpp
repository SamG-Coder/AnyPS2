#include <nt/inventory.hpp>
#include "reboot_fixture.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using fixture::put32;
using fixture::record;
void check(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
nt::Bytes make_elf() {
    nt::Bytes bytes(260,0);
    const std::uint8_t magic[]{0x7f,'E','L','F',1,1,1};
    std::copy(std::begin(magic),std::end(magic),bytes.begin());
    bytes[16]=2; bytes[18]=8; bytes[20]=1; bytes[40]=52; bytes[42]=32; bytes[44]=1;
    put32(bytes,24,0x100000); put32(bytes,28,52);
    put32(bytes,52,1); put32(bytes,56,256); put32(bytes,60,0x100000);
    put32(bytes,68,4); put32(bytes,72,8); put32(bytes,76,5); put32(bytes,80,4);
    return bytes;
}
nt::Bytes archive_of(const std::vector<std::pair<std::string,nt::Bytes>>& files) {
    const auto records=4+files.size();
    const auto directory=records*16;
    std::size_t cursor=directory;
    std::vector<std::size_t> places;
    for (const auto& file:files) {
        places.push_back(cursor);
        cursor+=(file.second.size()+15)&~std::size_t(15);
    }
    nt::Bytes image(cursor,0);
    const auto name_at=[&](std::size_t at,const std::string& name) {
        check(name.size()<=10,"archive name exceeds the directory field");
        std::copy(name.begin(),name.end(),image.begin()+at);
    };
    name_at(0,"RESET"); name_at(16,"ROMDIR"); name_at(32,"EXTINFO");
    put32(image,28,static_cast<std::uint32_t>(directory));
    for (std::size_t index=0;index<files.size();++index) {
        name_at(48+index*16,files[index].first);
        put32(image,48+index*16+12,static_cast<std::uint32_t>(files[index].second.size()));
        std::copy(files[index].second.begin(),files[index].second.end(),image.begin()+places[index]);
    }
    return image;
}
nt::Bytes iso_with(const std::vector<std::pair<std::string,nt::Bytes>>& files) {
    std::uint32_t sector=21;
    struct Placed { std::uint32_t sector,size; };
    std::vector<Placed> placed;
    for (const auto& file:files) {
        const auto sectors=static_cast<std::uint32_t>((file.second.size()+2047)/2048);
        placed.push_back({sector,static_cast<std::uint32_t>(file.second.size())});
        sector+=std::max<std::uint32_t>(sectors,1);
    }
    nt::Bytes iso(static_cast<std::size_t>(sector)*2048,0);
    const auto pvd=16*2048;
    iso[pvd]=1;
    const std::string signature="CD001";
    std::copy(signature.begin(),signature.end(),iso.begin()+pvd+1);
    iso[pvd+6]=1; iso[pvd+129]=8; iso[pvd+130]=8;
    const std::string volume="SYNTHETIC";
    std::copy(volume.begin(),volume.end(),iso.begin()+pvd+40);
    record(iso,pvd+156,std::string(1,'\0'),20,2048,true);
    std::size_t at=20*2048;
    for (std::size_t index=0;index<files.size();++index)
        at+=record(iso,at,files[index].first,placed[index].sector,placed[index].size,false);
    check(at<=21ull*2048,"synthetic directory exceeds one sector");
    for (std::size_t index=0;index<files.size();++index)
        std::copy(files[index].second.begin(),files[index].second.end(),iso.begin()+static_cast<std::size_t>(placed[index].sector)*2048);
    return iso;
}
const nt::inventory::Unit* find_unit(const nt::inventory::Inventory& inventory,std::string_view fragment) {
    for (const auto& unit:inventory.units) if (unit.id.find(fragment)!=std::string::npos) return &unit;
    return nullptr;
}
const nt::inventory::Unit* find_exact(const nt::inventory::Inventory& inventory,std::string_view id) {
    for (const auto& unit:inventory.units) if (unit.id==id) return &unit;
    return nullptr;
}
std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path,std::ios::binary);
    check(static_cast<bool>(input),"artifact file missing");
    return std::string(std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>());
}
nt::Bytes module_with(std::uint32_t instruction) {
    const auto image=fixture::memory_bootstrap_image();
    nt::Bytes module(image.begin()+80,image.end());
    put32(module,128,instruction);
    return module;
}
}
int main() {
    try {
        const auto image=fixture::memory_bootstrap_image();
        nt::Bytes good(image.begin()+80,image.end());
        auto bad=good;
        put32(bad,128,0x40000000);
        // Separator aliases and a shared 48-character prefix used to select one directory.
        const std::string packed="PACK.IMG;1";
        const std::string packed_alias="PACK_IMG_1";
        const auto prefix=std::string(60,'B');
        const auto long_one=prefix+"ONE.IMG;1";
        const auto long_two=prefix+"TWO.IMG;1";
        const std::string cnf="BOOT2 = cdrom0:\\BOOT.ELF;1\n";
        const auto iso=iso_with({
            {"SYSTEM.CNF;1",nt::Bytes(cnf.begin(),cnf.end())},
            {"BOOT.ELF;1",make_elf()},
            {"IOPRP.IMG;1",archive_of({{"SYSMEM",good},{"BADMOD",bad}})},
            {"NOTES.BIN;1",nt::Bytes{'n','o','t',' ','c','o','d','e'}},
            {packed,archive_of({{"FILE.A",module_with(0x24020007)},{"FILE_A",module_with(0x24020009)},{"SAME",module_with(0x2402000b)}})},
            {packed_alias,archive_of({{"SAME",module_with(0x2402000d)}})},
            {long_one,archive_of({{"EDGE",module_with(0x24020011)}})},
            {long_two,archive_of({{"EDGE",module_with(0x24020013)}})}
        });
        const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
        const auto root=std::filesystem::temp_directory_path()/("anyps2-conversion-inventory-"+std::to_string(stamp));
        std::filesystem::create_directories(root);
        const auto disc=root/"disc.iso";
        nt::write_file(disc,iso);
        nt::inventory::Options options;
        options.output=root/"out";
        options.compile=false;
        const auto inventory=nt::inventory::convert_disc(disc,options);
        const auto text=nt::inventory::format_report(inventory);
        check(text.find("linked=0")!=std::string::npos && text.find("executed=0")!=std::string::npos,
            "Inventory claimed link or execution coverage");
        check(text.find("not decoded")!=std::string::npos,"Unrecognized content was not reported as undecoded");
        const auto boot=find_unit(inventory,"BOOT.ELF");
        const auto good_module=find_unit(inventory,"SYSMEM");
        const auto bad_module=find_unit(inventory,"BADMOD");
        const auto notes=find_unit(inventory,"NOTES.BIN");
        check(boot && boot->kind=="ee-boot" && boot->stage==nt::inventory::Stage::analyzed,"Boot ELF was not analyzed independently");
        check(good_module && bad_module,"A failing archive member removed its sibling");
        check(bad_module->stage!=nt::inventory::Stage::llvm_generated && bad_module->stage!=nt::inventory::Stage::machine_compiled,
            "Unsupported IOP code was emitted");
        check(good_module->stage==nt::inventory::Stage::llvm_generated,"Supported IOP sibling was not converted");
        check(good_module->overlaps_native_service,"Native service overlap was not reported");
        check(notes && notes->kind=="unrecognized" && notes->stage==nt::inventory::Stage::discovered,
            "Unrecognized bytes were treated as executable content");
        check(std::any_of(inventory.gaps.begin(),inventory.gaps.end(),[](const auto& gap) { return gap.subsystem=="iop-instruction"; }),
            "Unsupported IOP instruction was not grouped");
        check(std::any_of(inventory.gaps.begin(),inventory.gaps.end(),[](const auto& gap) { return gap.subsystem=="ee-unmapped"; }),
            "EE discovery gap was not grouped");
        const auto llvm=read_text(good_module->artifact);
        check(llvm.find("nt_iop_llvm_run_v2")!=std::string::npos,"Converted IOP LLVM entry is missing");
        check(llvm.find("not code")==std::string::npos,"Unrecognized file bytes were copied into LLVM");
        check(read_text(good_module->artifact.parent_path()/"source.txt")==good_module->id,
            "Converted module lost its source identity");
        const auto require_distinct=[&](const std::string& id,const char* immediate) {
            const auto unit=find_exact(inventory,id);
            check(unit && unit->stage==nt::inventory::Stage::llvm_generated,"Colliding source was not converted on its own");
            const auto ir_text=read_text(unit->artifact);
            const auto marker=std::string("add i32 0, ")+immediate+"\n";
            check(ir_text.find(marker)!=std::string::npos,"Converted module lost its own instruction");
            check(read_text(unit->artifact.parent_path()/"source.txt")==unit->id,"Artifact provenance does not match the source");
            return unit;
        };
        const auto dotted=require_distinct(packed+"/FILE.A","7");
        const auto underscored=require_distinct(packed+"/FILE_A","9");
        const auto packed_same=require_distinct(packed+"/SAME","11");
        const auto alias_same=require_distinct(packed_alias+"/SAME","13");
        const auto first_long=require_distinct(long_one+"/EDGE","17");
        const auto second_long=require_distinct(long_two+"/EDGE","19");
        check(dotted->artifact!=underscored->artifact,"Sanitized member names shared an artifact directory");
        check(packed_same->artifact!=alias_same->artifact,"Sanitized archive names shared an artifact directory");
        check(first_long->artifact!=second_long->artifact,"Long source prefixes shared an artifact directory");
        check(read_text(dotted->artifact).find("add i32 0, 9\n")==std::string::npos,"Sanitized member artifact contains the other module");
        check(read_text(first_long->artifact).find("add i32 0, 19\n")==std::string::npos,"Long-prefix artifact contains the other module");
        bool refused=false;
        try { nt::inventory::convert_disc(disc,options); } catch (const std::exception&) { refused=true; }
        check(refused,"Existing inventory output was overwritten");
        std::error_code cleanup;
        std::filesystem::remove_all(root,cleanup);
        std::cout<<"Conversion inventory covered independent modules, undecoded files and cumulative stages\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<error.what()<<'\n';
        return 1;
    }
}
