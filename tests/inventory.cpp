#include <nt/inventory.hpp>
#include "reboot_fixture.hpp"
#include "vector_section_fixture.hpp"
#include <algorithm>
#include <cctype>
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
std::string read_ir(const nt::inventory::Unit& unit) {
    std::string text;
    for (const auto& module:unit.modules) text+=read_text(module);
    return text;
}
nt::Bytes module_with(std::uint32_t instruction) {
    const auto image=fixture::memory_bootstrap_image();
    nt::Bytes module(image.begin()+80,image.end());
    put32(module,128,instruction);
    return module;
}
}
int main(int argc,char** argv) {
    try {
        if ((argc==3 || argc==4) && std::string_view(argv[1])=="--emit-fixture") {
            auto boot=make_elf(); boot.resize(268);
            put32(boot,68,12); put32(boot,72,12);
            put32(boot,256,0x2402002b); put32(boot,260,0x2403007b); put32(boot,264,12);
            const std::string cnf="BOOT2 = cdrom0:\\BOOT.ELF;1\n";
            std::vector<std::pair<std::string,nt::Bytes>> files{
                {"SYSTEM.CNF;1",nt::Bytes(cnf.begin(),cnf.end())},{"BOOT.ELF;1",boot},
                {"MODULE.IMG;1",archive_of({{"FIRST",module_with(0x24020007)},{"SECOND",module_with(0x24020009)}})}
            };
            if (argc==4) {
                auto packed=nt::read_file(argv[3],65536);
                files.emplace_back("SECONDARY.BIN;1",packed);
                packed.pop_back(); files.emplace_back("TRUNCATED.BIN;1",std::move(packed));
            }
            nt::write_file(argv[2],iso_with(files));
            return 0;
        }
        {
            auto bytes=make_elf(); bytes.resize(320);
            put32(bytes,68,64); put32(bytes,72,64);
            put32(bytes,256,0x40840000); put32(bytes,260,12);
            put32(bytes,272,0x24030055); put32(bytes,276,12);
            put32(bytes,280,0x03e00008);
            const auto coverage=nt::ee_codegen::assess(bytes);
            check(coverage.wrapper_candidates==1 && coverage.known_syscalls.contains(0x55),
                "Unreached syscall absent from batch coverage");
            check(coverage.address_space_requests==1 && coverage.first_address_space_request==0x100000,
                "Emitted address-space operation was mistaken for implemented native memory");
            check(coverage.unknown_syscall_sites==1 && coverage.convertible,
                "Unknown syscall boundary or structural compilation coverage lost");
        }
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
            {packed,archive_of({{"FILE.A",module_with(0x24020007)},{"FILE_A",module_with(0x24020009)},{"SAME",module_with(0x2402000b)},
                {"Ab",module_with(0x24020015)},{"AB",module_with(0x24020017)}})},
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
        check(std::filesystem::exists(options.output/"library-calls.json"),"Call inventory was not emitted");
        {
            const auto vector_elf=fixture::vector_container_elf();
            auto branch=vector_elf; put32(branch,0x100,0x08000418);
            const auto rejected=nt::ee_codegen::assess(branch);
            check(!rejected.convertible && rejected.unresolved_count==1 && rejected.unresolved_sample[0]==0x1060,
                "An actual EE jump into a VU container was silently discarded");
            const auto vector_disc=root/"vector.iso";
            nt::write_file(vector_disc,iso_with({{"SYSTEM.CNF;1",nt::Bytes(cnf.begin(),cnf.end())},
                {"BOOT.ELF;1",vector_elf}}));
            auto vector_options=options; vector_options.output=root/"vector";
            const auto vector_inventory=nt::inventory::convert_disc(vector_disc,vector_options);
            const auto* ee=find_exact(vector_inventory,"BOOT.ELF;1");
            const auto* vu=find_exact(vector_inventory,"BOOT.ELF;1/.vutext[0]");
            check(ee && ee->stage==nt::inventory::Stage::llvm_generated && ee->instructions==5,
                "A stored VU payload pointer was compiled as an EE function");
            check(vu && vu->stage==nt::inventory::Stage::parsed && vu->modules.empty() && vu->analysis_base==0x1060,
                "VU container was hidden or falsely reported as converted");
            check(std::any_of(vector_inventory.gaps.begin(),vector_inventory.gaps.end(),[](const auto& gap) {
                return gap.subsystem=="vu-container";
            }),"Separate VU conversion requirement was not reported");
        }
        {
            auto audit_options=options;
            audit_options.output=root/"audit"; audit_options.generate=false;
            const auto audit=nt::inventory::convert_disc(disc,audit_options);
            check(audit.calls.size()==inventory.calls.size(),"Analysis-only mode lost library calls");
            check(std::all_of(audit.units.begin(),audit.units.end(),[](const auto& unit) {
                return unit.modules.empty() && unit.stage<=nt::inventory::Stage::analyzed;
            }),"Analysis-only mode generated code");
        }
        {
            using namespace nt::inventory;
            const auto client=[](std::string_view library,std::uint16_t version,std::initializer_list<std::uint16_t> ordinals) {
                nt::elf::IopModule module{0x1000,0x1180,nt::Bytes(512),512,{}};
                put32(module.memory,0,0x41e00000); put32(module.memory,8,version);
                std::copy(library.begin(),library.end(),module.memory.begin()+12);
                unsigned at=20;
                for (const auto ordinal:ordinals) {
                    put32(module.memory,at,0x03e00008); put32(module.memory,at+4,0x24000000|ordinal); at+=8;
                }
                put32(module.memory,0x180,0x03e00008);
                return module;
            };
            auto native=client("sysclib",0x104,{12,11,65535});
            auto imported=client("sample",0x101,{0});
            auto provider=client("unused",0x101,{});
            put32(provider.memory,0x80,0x41c00000); put32(provider.memory,0x88,0x102);
            const std::string sample="sample";
            std::copy(sample.begin(),sample.end(),provider.memory.begin()+0x8c);
            put32(provider.memory,0x94,0x1180);
            auto incompatible=provider; put32(incompatible.memory,0x88,0x201);
            const std::vector modules{native,imported,provider,incompatible};
            const std::vector<std::string> sources{"client|quoted\".irx","caller.irx","provider.irx","incompatible.irx"};
            Inventory calls;
            record_iop_calls(calls,modules,sources,{});
            check(calls.calls.size()==4,"Call inventory lost declared or unresolved imports");
            check(calls.calls[0].binding==CallBinding::intrinsic && !calls.calls[0].reached,
                "Unreached intrinsic import was missing or falsely marked executed");
            check(calls.calls[2].binding==CallBinding::unresolved,"Unknown ordinal was assigned a native provider");
            check(calls.calls[3].binding==CallBinding::compiled_export && calls.calls[3].providers.size()==1 &&
                calls.calls[3].providers[0].source=="provider.irx" && !calls.calls[3].providers[0].convertible,
                "Export discovery accepted an incompatible version or claimed compilation");
            auto unknown=calls.calls[0]; unknown.domain="ee"; unknown.library="kernel";
            unknown.version=0; unknown.binding=CallBinding::unknown; unknown.address.reset(); calls.calls.push_back(unknown);
            write_call_reports(calls,root);
            const auto json=read_text(root/"library-calls.json");
            const auto markdown=read_text(root/"library-calls.md");
            check(json.find("\"name\":\"memcpy\"")!=json.npos && json.find("\"selector\":65535")!=json.npos &&
                json.find("\"selector\":null")!=json.npos && json.find("\"runtime_verified\":false")!=json.npos &&
                json.find("client|quoted\\\".irx")!=json.npos,"Call report lost names, unknowns or source escaping");
            check(markdown.find("llvm.memcpy")!=markdown.npos && markdown.find("No provider is automatically installed")!=markdown.npos,
                "Native mapping report lost its compatibility boundary");
        }
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
            if (!unit || unit->stage!=nt::inventory::Stage::llvm_generated)
                throw std::runtime_error(std::string("Colliding source was not converted on its own: ")+std::string(id)+
                    (unit ? " stage="+std::to_string(static_cast<int>(unit->stage))+" "+unit->detail : " missing"));
            const auto ir_text=read_ir(*unit);
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
        const auto lower_case=require_distinct(packed+"/Ab","21");
        const auto upper_case=require_distinct(packed+"/AB","23");
        check(dotted->artifact!=underscored->artifact,"Sanitized member names shared an artifact directory");
        check(packed_same->artifact!=alias_same->artifact,"Sanitized archive names shared an artifact directory");
        check(first_long->artifact!=second_long->artifact,"Long source prefixes shared an artifact directory");
        const auto fold=[](std::filesystem::path path) {
            auto text=path.generic_string();
            for (auto& character:text) character=static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
            return text;
        };
        const auto identity_is_lower=[](const std::filesystem::path& artifact) {
            for (const auto& part:artifact) {
                const auto name=part.generic_string();
                if (name.empty() || name.front()!='_') continue;
                if (name.find_first_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ")!=std::string::npos) return false;
            }
            return true;
        };
        check(fold(lower_case->artifact)!=fold(upper_case->artifact),
            "Case-only member names shared an artifact directory");
        check(identity_is_lower(lower_case->artifact) && identity_is_lower(upper_case->artifact),
            "Case-distinct artifact identity is not lowercase");
        check(read_ir(*lower_case).find("add i32 0, 23\n")==std::string::npos,
            "Lowercase member artifact contains the uppercase module");
        check(read_ir(*upper_case).find("add i32 0, 21\n")==std::string::npos,
            "Uppercase member artifact contains the lowercase module");
        check(read_ir(*dotted).find("add i32 0, 9\n")==std::string::npos,"Sanitized member artifact contains the other module");
        check(read_ir(*first_long).find("add i32 0, 19\n")==std::string::npos,"Long-prefix artifact contains the other module");
#ifdef ANYPS2_TEST_CLANG
        // One failed object must not hide a supported sibling, and a partially
        // compiled unit must never receive machine-compiled status.
        nt::inventory::Inventory batch;
        batch.units={*good_module,*dotted};
        const auto malformed=root/"malformed.ll";
        { std::ofstream file(malformed); file<<"invalid LLVM input\n"; }
        batch.units[0].modules.push_back(malformed);
        options.compile=true; options.compiler=ANYPS2_TEST_CLANG; options.jobs=2;
        nt::inventory::compile_inventory(batch,options);
        check(batch.units[0].stage==nt::inventory::Stage::llvm_generated &&
            batch.units[0].compiled_objects+1==batch.units[0].modules.size(),"Partial object compilation was reported as complete");
        check(batch.units[1].stage==nt::inventory::Stage::machine_compiled &&
            batch.units[1].compiled_objects==batch.units[1].modules.size(),"A compiler failure prevented a supported sibling from compiling");
        check(std::any_of(batch.gaps.begin(),batch.gaps.end(),[](const auto& gap) { return gap.subsystem=="llvm-compile"; }),
            "Failed LLVM object was absent from the missing-work report");
#endif
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
