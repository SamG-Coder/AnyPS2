#include <nt/inventory.hpp>
#include <nt/ee_syscalls.hpp>
#include <nt/elf.hpp>
#include <nt/iop_codegen.hpp>
#include <nt/iop_module.hpp>
#include <nt/iso9660.hpp>
#include <nt/packed_image.hpp>
#include <nt/process.hpp>
#include <nt/reboot_image.hpp>
#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>
#include <string_view>
#include <fstream>
#include <memory>
#include <chrono>

namespace nt::inventory {
namespace {
constexpr std::uint64_t executable_read_limit=32ull*1024*1024;
constexpr std::uint64_t archive_read_limit=16ull*1024*1024;
constexpr std::size_t unit_limit=4096;
constexpr std::size_t unrecognized_unit_limit=64;
constexpr unsigned directory_depth_limit=6;

struct GroupKey {
    std::string subsystem;
    std::string key;
    bool operator<(const GroupKey& other) const {
        if (subsystem!=other.subsystem) return subsystem<other.subsystem;
        return key<other.key;
    }
};
struct GroupState {
    std::size_t count=0;
    std::uint32_t first=0;
    std::string detail;
    std::vector<std::string> owners;
};
struct Accumulator {
    Inventory inventory;
    std::map<GroupKey,GroupState> groups;
    std::size_t unrecognized=0;
    void limit(std::string text) { inventory.limits.push_back(std::move(text)); }
    void gap(std::string subsystem,std::string key,std::size_t count,std::uint32_t address,std::string owner,std::string detail={}) {
        if (!count) return;
        auto& state=groups[{std::move(subsystem),std::move(key)}];
        if (!state.count) { state.first=address; state.detail=std::move(detail); }
        state.count+=count;
        if (!owner.empty() &&
            std::find(state.owners.begin(),state.owners.end(),owner)==state.owners.end())
            state.owners.push_back(std::move(owner));
    }
    void finish() {
        for (auto& [key,state]:groups) {
            std::ostringstream owners;
            for (std::size_t index=0;index<state.owners.size();++index) {
                if (index) owners<<", ";
                owners<<state.owners[index];
            }
            inventory.gaps.push_back({key.subsystem,key.key,state.detail,owners.str(),state.count,state.first});
        }
        std::sort(inventory.gaps.begin(),inventory.gaps.end(),[](const auto& left,const auto& right) {
            if (left.subsystem!=right.subsystem) return left.subsystem<right.subsystem;
            if (left.count!=right.count) return left.count>right.count;
            return left.key<right.key;
        });
        std::sort(inventory.units.begin(),inventory.units.end(),[](const auto& left,const auto& right) { return left.id<right.id; });
    }
};

std::string stem(std::string value) {
    std::replace(value.begin(),value.end(),'\\','/');
    value=value.substr(0,value.find(';'));
    for (auto& character:value) character=static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    return value;
}
// Filesystem identity of one source name. Digits, letters and '-' are copied in
// lowercase. Each group of four source bytes adds one hex digit whose bits record
// which of those bytes were uppercase, so a case-insensitive filesystem cannot
// merge names that differ only by case. Every other byte, including '_' and
// separators, becomes '_' plus two lowercase hex digits. '=' separates the case
// mask and is not produced by the body. The decimal length prefix makes the
// encoding injective, including the empty name. No hash is used.
std::string encode_identity(std::string_view text) {
    std::string encoded=std::to_string(text.size());
    encoded.push_back('_');
    static constexpr char hex[]="0123456789abcdef";
    unsigned bits=0, count=0;
    std::string cases;
    for (unsigned char character:text) {
        if (character>='A' && character<='Z') {
            encoded.push_back(static_cast<char>(character-'A'+'a'));
            bits|=1u<<count;
        } else if ((character>='0' && character<='9') || (character>='a' && character<='z') || character=='-') {
            encoded.push_back(static_cast<char>(character));
        } else {
            encoded.push_back('_');
            encoded.push_back(hex[character>>4]);
            encoded.push_back(hex[character&0x0f]);
        }
        if (++count==4) { cases.push_back(hex[bits]); bits=0; count=0; }
    }
    if (count) cases.push_back(hex[bits]);
    encoded.push_back('=');
    encoded+=cases;
    return encoded;
}
// Split a long identity into nested components. A leading '_' keeps a short final
// chunk away from Windows reserved device names. Fixed slices of an injective
// string stay injective.
std::filesystem::path identity_directory(std::string_view text) {
    const auto encoded=encode_identity(text);
    std::filesystem::path path;
    constexpr std::size_t chunk=80;
    for (std::size_t offset=0;offset<encoded.size();offset+=chunk)
        path/="_" + encoded.substr(offset,std::min(chunk,encoded.size()-offset));
    return path;
}
bool native_library(std::string_view name) {
    std::string lower(name);
    for (auto& character:lower) character=static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    static constexpr std::string_view names[]{"romdrv","modload","loadcore","intrman","sifcmd","stdio","sifman","thbase",
        "thsemap","thmsgbx","thevent","ioman","sysmem","heaplib","sysclib"};
    return std::find(std::begin(names),std::end(names),std::string_view(lower))!=std::end(names);
}
bool looks_elf(ByteView header) {
    static constexpr std::uint8_t magic[]{0x7f,'E','L','F',1,1,1};
    return header.size()>=sizeof(magic) && std::equal(std::begin(magic),std::end(magic),header.begin());
}
bool looks_reboot(ByteView header) {
    const auto text=reinterpret_cast<const char*>(header.data());
    return header.size()>=22 && std::string_view(text,5)=="RESET" && std::string_view(text+16,6)=="ROMDIR";
}
void write_text(const std::filesystem::path& path,const std::string& text) {
    write_file(path,ByteView(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()));
}
std::string hex_list(const std::vector<std::uint32_t>& values) {
    std::ostringstream text;
    for (std::size_t index=0;index<values.size();++index) {
        if (index) text<<", ";
        text<<hex32(values[index]);
    }
    return text.str();
}
void record_iop_gaps(Accumulator& accumulator,const std::string& id,const elf::IopModule& module,const iop_codegen::ir::ModuleDiagnosis& diagnosis) {
    const auto extent=module.info ? module.info->text_size : module.initialized_size;
    const auto declared=extent/4;
    if (declared>diagnosis.program.instructions.size())
        accumulator.gap("iop-unreached","declared text words without an analyzed instruction",
            declared-diagnosis.program.instructions.size(),module.base,id,
            "Includes data placed in the text extent and code not reached from the entry, exports or discovered callbacks.");
    for (const auto& item:diagnosis.gaps) {
        std::string subsystem="iop-control";
        std::string key=item.detail;
        switch (item.kind) {
        case iop_codegen::ir::Gap::Kind::import_missing:
            subsystem="iop-import";
            key=item.library+" "+std::to_string(item.version>>8)+"."+std::to_string(item.version&255)+
                " #"+std::to_string(item.ordinal);
            break;
        case iop_codegen::ir::Gap::Kind::instruction: subsystem="iop-instruction"; break;
        case iop_codegen::ir::Gap::Kind::indirect: subsystem="iop-indirect"; key="through "+item.detail; break;
        case iop_codegen::ir::Gap::Kind::unresolved_target: subsystem="iop-target"; break;
        case iop_codegen::ir::Gap::Kind::limit: subsystem="iop-limit"; break;
        case iop_codegen::ir::Gap::Kind::control: break;
        }
        accumulator.gap(std::move(subsystem),key.empty()?item.detail:key,1,item.address,id);
    }
    if (!diagnosis.unexpected.empty())
        accumulator.gap("iop-analysis",diagnosis.unexpected.substr(0,180),1,module.base,id);
}
void emit_iop(Unit& unit,const iop_codegen::ir::Program& program,const std::filesystem::path& directory) {
    if (std::filesystem::exists(directory)) throw std::runtime_error("IOP artifact directory already exists");
    std::filesystem::create_directories(directory);
    const auto ir=directory/"generated.ll";
    std::ofstream llvm(ir,std::ios::binary); llvm.exceptions(std::ios::badbit|std::ios::failbit);
    std::vector<std::unique_ptr<std::ofstream>> streams;
    unit.modules.push_back(ir);
    iop_codegen::emit_program(llvm,program,[&](std::size_t group)->std::ostream& {
        const auto file=directory/("iop-code-"+std::to_string(group)+".ll");
        auto stream=std::make_unique<std::ofstream>(file,std::ios::binary);
        stream->exceptions(std::ios::badbit|std::ios::failbit);
        unit.modules.push_back(file); streams.push_back(std::move(stream)); return *streams.back();
    });
    llvm.close(); for (auto& stream:streams) stream->close();
    std::string manifest;
    for (const auto& file:unit.modules) manifest+=file.filename().string()+'\n';
    write_text(directory/"llvm-modules.txt",manifest);
    write_text(directory/"source.txt",unit.id);
    write_text(directory/"conversion.txt",
        "AnyPS2 IOP module LLVM inventory\n"
        "This file is machine code only after a separate clang compilation.\n"
        "It is not linked into a runtime and it was not executed.\n"
        "Analysis base "+hex32(unit.analysis_base)+" is an inventory placement, not a disc address.\n");
    unit.artifact=ir;
    unit.stage=Stage::llvm_generated;

}
void convert_iop_group(Accumulator& accumulator,const std::string& owner,std::vector<std::pair<std::string,elf::IopModule>> modules,const std::filesystem::path& output,bool generate) {
    std::vector<elf::IopModule> loaded;
    std::vector<std::string> names;
    loaded.reserve(modules.size());
    for (auto& [name,module]:modules) {
        names.push_back(name);
        loaded.push_back(std::move(module));
    }
    std::vector<iop_codegen::ir::ModuleDiagnosis> diagnosed;
    std::string bundle_error;
    try { diagnosed=iop_codegen::ir::diagnose_bundle(loaded); }
    catch (const std::exception& error) { bundle_error=error.what(); }
    std::vector<std::string> sources;
    for (const auto& name:names) sources.push_back(owner+"/"+name);
    record_iop_calls(accumulator.inventory,loaded,sources,diagnosed);
    if (!bundle_error.empty()) {
        accumulator.gap("iop-analysis",bundle_error.substr(0,180),1,0,owner);
        for (std::size_t index=0;index<loaded.size();++index) {
            Unit unit;
            unit.id=owner+"/"+names[index];
            unit.kind="iop-module";
            unit.stage=Stage::parsed;
            unit.analysis_base=loaded[index].base;
            unit.detail=bundle_error.substr(0,500);
            accumulator.inventory.units.push_back(std::move(unit));
        }
        return;
    }
    for (std::size_t index=0;index<loaded.size();++index) {
        const auto& module=loaded[index];
        const auto& diagnosis=diagnosed[index];
        Unit unit;
        unit.id=owner+"/"+names[index];
        unit.kind="iop-module";
        unit.stage=Stage::analyzed;
        unit.analysis_base=module.base;
        unit.instructions=diagnosis.program.instructions.size();
        unit.declared_words=(module.info ? module.info->text_size : module.initialized_size)/4;
        if (native_library(names[index])) unit.overlaps_native_service=true;
        try {
            const auto exports=elf::iop_exports(module);
            for (const auto& library:exports) if (native_library(library.name)) unit.overlaps_native_service=true;
            // ROMDRV 2.x with the six-entry archive ABI is the one export shape
            // the native archive service actually replaces. An export signature
            // alone is not a provider, so other libraries stay ordinary modules.
            unit.native_replacement=exports.size()==1 && exports.front().name=="romdrv" && (exports.front().version>>8)==2 &&
                exports.front().functions.size()==6;
        } catch (const std::exception&) {}
        record_iop_gaps(accumulator,unit.id,module,diagnosis);
        if (!diagnosis.unexpected.empty()) unit.detail=diagnosis.unexpected.substr(0,500);
        else if (!diagnosis.gaps.empty()) unit.detail=std::to_string(diagnosis.gaps.size())+" conversion gap(s)";
        if (unit.overlaps_native_service) {
            if (!unit.detail.empty()) unit.detail+="; ";
            unit.detail+="export name matches a native service library; not linked";
        }
        if (unit.native_replacement) {
            if (!unit.detail.empty()) unit.detail+="; ";
            unit.detail+="ROMDRV 2.x six-entry ABI matches the native archive replacement";
        }
        if (generate && diagnosis.convertible()) {
            try {
                emit_iop(unit,diagnosis.program,output/"iop"/identity_directory(owner)/identity_directory(names[index]));
            } catch (const std::exception& error) {
                if (!unit.detail.empty()) unit.detail+="; ";
                unit.detail+=error.what();
            }
        }
        accumulator.inventory.units.push_back(std::move(unit));
    }
}
void record_ee(Accumulator& accumulator,const std::string& id,ByteView bytes,const std::filesystem::path& output,const std::vector<ee_codegen::PayloadMapping>& payloads,bool boot,bool packed=false,bool generate=true) {
    Unit unit;
    unit.id=id;
    unit.kind=boot?"ee-boot":packed?"ee-packed":"ee-elf";
    try {
        const auto executable=elf::parse(bytes);
        for (std::size_t index=0;index<executable.vector_sections.size();++index) {
            const auto& section=executable.vector_sections[index];
            Unit vector_unit;
            vector_unit.id=id+"/.vutext["+std::to_string(index)+"]";
            vector_unit.kind="vu-container";
            vector_unit.stage=Stage::parsed;
            vector_unit.analysis_base=section.address;
            vector_unit.detail=std::to_string(section.size)+" bytes of VU payload storage; upload framing and VU entry points are not yet recovered";
            accumulator.gap("vu-container","ELF VU section requires separate conversion",1,section.address,
                vector_unit.id,"Preserved in the input image and excluded from EE callback discovery. Not compiled or executed.");
            accumulator.inventory.units.push_back(std::move(vector_unit));
        }
        unit.stage=Stage::parsed;
    } catch (const std::exception& error) {
        unit.detail=error.what();
        accumulator.gap("ee-parse",unit.detail.substr(0,180),1,0,id);
        accumulator.inventory.units.push_back(std::move(unit));
        return;
    }
    const auto assessment=ee_codegen::assess(bytes,payloads);
    if (!assessment.layout_error.empty()) {
        unit.detail=assessment.layout_error.substr(0,500);
        accumulator.gap("ee-layout",unit.detail.substr(0,180),1,0,id);
        accumulator.inventory.units.push_back(std::move(unit));
        return;
    }
    unit.stage=Stage::analyzed;
    unit.instructions=assessment.instructions;
    unit.declared_words=assessment.declared_executable_words;
    if (assessment.declared_executable_words>assessment.instructions)
        accumulator.gap("ee-unreached","declared executable file words not reached by discovery",
            assessment.declared_executable_words-assessment.instructions,0,id,
            "Indirect, encrypted, transformed and dynamically loaded targets stay in this gap until a real root is recovered.");
    if (assessment.unresolved_count)
        accumulator.gap("ee-unmapped","discovery target is not file-backed executable memory",assessment.unresolved_count,
            assessment.unresolved_sample.empty()?0:assessment.unresolved_sample.front(),id,hex_list(assessment.unresolved_sample));
    if (assessment.unclassified_pointer_count)
        accumulator.gap("ee-pointer-reference","stored/passed address has no recovered call consumer",
            assessment.unclassified_pointer_count,assessment.unclassified_pointer_sample.front(),id,
            "Not established as EE code; payload/data uses and unrecovered callbacks remain distinct from compiled coverage. Candidates: "+
            hex_list(assessment.unclassified_pointer_sample));
    if (assessment.indirect_sites)
        accumulator.gap("ee-indirect","register-indirect transfer",assessment.indirect_sites,assessment.first_indirect,id,
            "Compiled indirect sites stop at runtime when the destination was not discovered.");
    if (assessment.unknown_syscall_sites)
        accumulator.gap("ee-syscall","number encoding was not a recognized immediate",assessment.unknown_syscall_sites,0,id);
    if (assessment.unknown_syscall_sites) {
        LibraryCall call;
        call.domain="ee"; call.library="kernel"; call.source=id;
        call.sites=assessment.unknown_syscall_sites;
        accumulator.inventory.calls.push_back(std::move(call));
    }
    if (assessment.address_space_requests)
        accumulator.gap("ee-native-memory","conversion-time native memory lowering required",
            assessment.address_space_requests,assessment.first_address_space_request,id,
            "LLVM instruction emission is available, but no production mapping implementation is supplied. No runtime MMU fallback.");
    unit.detail="leaf syscall-wrapper candidates="+std::to_string(assessment.wrapper_candidates);
    for (const auto& [number,count]:assessment.known_syscalls) {
        std::ostringstream key;
        const auto registered=std::find(assessment.registered_calls.begin(),assessment.registered_calls.end(),number)!=assessment.registered_calls.end();
        LibraryCall call;
        call.domain="ee"; call.library="kernel"; call.source=id; call.selector=number; call.sites=count;
        call.binding=registered ? CallBinding::registered_handler :
            ee::startup_syscall(number) ? CallBinding::native_dispatch : CallBinding::unresolved;
        accumulator.inventory.calls.push_back(std::move(call));
        key<<hex32(number)<<(registered ? " registered-handler-candidate" :
            ee::startup_syscall(number) ? " native-dispatch" : " no-native-dispatch");
        accumulator.gap("ee-syscall",key.str(),count,number,id,
            "Static candidate only, not proof of runtime use or success. Native dispatch still validates arguments and providers; registered handlers must execute compiled code. Missing dispatch needs a native implementation or a proven registered handler.");
    }
    for (const auto& family:assessment.rejection.families)
        accumulator.gap("ee-opcode",family.name,family.count,family.first,id);
    for (const auto& constraint:assessment.rejection.constraints) {
        const auto at=constraint.rfind(" at ");
        accumulator.gap("ee-encoding",at==std::string::npos?constraint:constraint.substr(0,at),1,0,id);
    }
    if (assessment.rejection.omitted_constraints)
        accumulator.gap("ee-encoding","additional encoding constraints omitted",assessment.rejection.omitted_constraints,0,id);
    if (!assessment.convertible) unit.detail="reached code is not fully convertible";
    if (generate && assessment.convertible) {
        try {
            const auto directory=output/"ee"/identity_directory(id);
            std::filesystem::create_directories(directory);
            write_file(directory/"input.elf",bytes);
            const auto project=directory/"project";
            ee_codegen::project(directory/"input.elf",project,payloads);
            write_text(project/"source.txt",unit.id);
            unit.artifact=project/"generated.ll";
            unit.stage=Stage::llvm_generated;
            std::ifstream manifest(project/"llvm-modules.txt");
            std::string module;
            while (std::getline(manifest,module)) if (!module.empty()) unit.modules.push_back(project/module);
            if (unit.modules.empty()) throw std::runtime_error("Generated EE project has no LLVM modules");
        } catch (const std::exception& error) { unit.detail=error.what(); }
    }
    accumulator.inventory.units.push_back(std::move(unit));
}
void consider_file(Accumulator& accumulator,disc::Iso9660& disc,const disc::Entry& entry,const std::string& path,const std::filesystem::path& output,const Options& options,const std::string& boot) {
    if (accumulator.inventory.units.size()>=unit_limit) {
        accumulator.limit("File unit limit reached; later disc entries were not inventoried.");
        return;
    }
    ByteView header;
    Bytes header_bytes;
    try {
        header_bytes=disc.read_range(entry,0,static_cast<std::uint32_t>(std::min<std::uint64_t>(entry.size,64)));
        header=header_bytes;
    } catch (const std::exception& error) {
        Unit unit; unit.id=path; unit.kind="unreadable"; unit.detail=error.what();
        accumulator.inventory.units.push_back(std::move(unit));
        return;
    }
    const bool boot_file=!boot.empty() && stem(path)==stem(boot);
    if (looks_elf(header)) {
        if (entry.size>executable_read_limit) {
            Unit unit; unit.id=path; unit.kind="oversized-elf"; unit.detail="ELF exceeds the 32 MiB inventory read";
            accumulator.gap("undiscovered","ELF exceeds the read bound",1,0,path);
            accumulator.inventory.units.push_back(std::move(unit));
            return;
        }
        Bytes bytes;
        try { bytes=disc.read(entry,executable_read_limit); }
        catch (const std::exception& error) {
            Unit unit; unit.id=path; unit.kind="unreadable"; unit.detail=error.what();
            accumulator.inventory.units.push_back(std::move(unit));
            return;
        }
        const auto type=le16(bytes,16);
        if (type==0xff80) {
            try {
                auto module=elf::load_iop_module(bytes,0x10000);
                const auto slash=path.find_last_of('/');
                const auto leaf=slash==std::string::npos ? path : path.substr(slash+1);
                std::vector<std::pair<std::string,elf::IopModule>> modules;
                modules.emplace_back(stem(leaf),std::move(module));
                convert_iop_group(accumulator,path,std::move(modules),output,options.generate);
            } catch (const std::exception& error) {
                Unit unit; unit.id=path; unit.kind="iop-module"; unit.detail=error.what();
                accumulator.gap("iop-parse",std::string(error.what()).substr(0,180),1,0,path);
                accumulator.inventory.units.push_back(std::move(unit));
            }
            return;
        }
        if (type==2) {
            record_ee(accumulator,path,bytes,output,options.payloads,boot_file,false,options.generate);
            return;
        }
        Unit unit; unit.id=path; unit.kind="elf-other"; unit.detail="ELF type "+std::to_string(type)+" is not an executable or IRX";
        accumulator.gap("undiscovered",unit.detail,1,0,path);
        accumulator.inventory.units.push_back(std::move(unit));
        return;
    }
    if (looks_reboot(header)) {
        if (entry.size>archive_read_limit) {
            Unit unit; unit.id=path; unit.kind="oversized-archive"; unit.detail="module archive exceeds the 16 MiB inventory read";
            accumulator.gap("undiscovered","module archive exceeds the read bound",1,0,path);
            accumulator.inventory.units.push_back(std::move(unit));
            return;
        }
        Bytes bytes;
        try { bytes=disc.read(entry,archive_read_limit); }
        catch (const std::exception& error) {
            Unit unit; unit.id=path; unit.kind="unreadable"; unit.detail=error.what();
            accumulator.inventory.units.push_back(std::move(unit));
            return;
        }
        std::vector<disc::RebootFile> members;
        try { members=disc::parse_reboot_image(bytes); }
        catch (const std::exception& error) {
            Unit unit; unit.id=path; unit.kind="unrecognized"; unit.detail=error.what();
            accumulator.gap("undiscovered","RESET header did not parse as a disc module archive",1,0,path);
            accumulator.inventory.units.push_back(std::move(unit));
            return;
        }
        Unit archive; archive.id=path; archive.kind="module-archive"; archive.stage=Stage::parsed;
        archive.detail=std::to_string(members.size())+" archive member(s); non-ELF members were not decoded";
        accumulator.inventory.units.push_back(std::move(archive));
        std::vector<std::pair<std::string,elf::IopModule>> modules;
        std::uint32_t base=0x10000;
        for (const auto& member:members) {
            const auto id=path+"/"+member.name;
            if (member.size<4 || member.offset>bytes.size() || member.size>bytes.size()-member.offset || le32(bytes,member.offset)!=0x464c457f) {
                if (member.name=="RESET" || member.name=="ROMDIR" || member.name=="EXTINFO") continue;
                Unit unit; unit.id=id; unit.kind="archive-data"; unit.detail="archive member is not an ELF";
                accumulator.gap("undiscovered","archive member is not an ELF",1,0,id);
                accumulator.inventory.units.push_back(std::move(unit));
                continue;
            }
            const auto file=ByteView(bytes).subspan(member.offset,member.size);
            try {
                auto module=elf::load_iop_module(file,base);
                const auto next=(base+static_cast<std::uint32_t>(module.memory.size())+255u)&~255u;
                if (next<base || next>0x1e0000) {
                    Unit unit; unit.id=id; unit.kind="iop-module"; unit.stage=Stage::parsed; unit.analysis_base=base;
                    unit.detail="module does not fit in the shared inventory map below 0x1e0000";
                    accumulator.gap("iop-layout",unit.detail,1,base,id);
                    accumulator.inventory.units.push_back(std::move(unit));
                    continue;
                }
                base=next;
                modules.emplace_back(member.name,std::move(module));
            } catch (const std::exception& error) {
                Unit unit; unit.id=id; unit.kind="iop-module"; unit.detail=error.what();
                accumulator.gap("iop-parse",std::string(error.what()).substr(0,180),1,0,id);
                accumulator.inventory.units.push_back(std::move(unit));
            }
        }
        if (!modules.empty()) convert_iop_group(accumulator,path,std::move(modules),output,options.generate);
        return;
    }
    // Full container validation must precede code discovery. The opaque prefix
    // and filename do not select behavior, and failed probes stay unrecognized.
    if (entry.size<=executable_read_limit && header.size()>=6 &&
        le32(header,2)>=24 && le32(header,2)<=executable_read_limit) {
        Bytes unpacked;
        try { unpacked=package::elf_image(package::unpack(disc.read(entry,executable_read_limit))); }
        catch (const std::exception&) { /* Not a recognized executable container. */ }
        if (!unpacked.empty()) {
            record_ee(accumulator,path,unpacked,output,{},boot_file,true,options.generate);
            return;
        }
    }
    if (accumulator.unrecognized>=unrecognized_unit_limit) {
        ++accumulator.unrecognized;
        accumulator.gap("undiscovered","not an ELF or disc module archive",1,0,path);
        return;
    }
    ++accumulator.unrecognized;
    Unit unit; unit.id=path; unit.kind="unrecognized";
    std::ostringstream detail;
    detail<<"not an ELF or disc module archive; "<<entry.size<<" bytes; header";
    for (std::size_t index=0;index<header.size() && index<8;++index)
        detail<<' '<<std::hex<<std::uppercase<<static_cast<unsigned>(header[index]);
    unit.detail=detail.str();
    accumulator.gap("undiscovered","not an ELF or disc module archive",1,0,path);
    accumulator.inventory.units.push_back(std::move(unit));
}
void walk(Accumulator& accumulator,disc::Iso9660& disc,const disc::Entry& directory,const std::string& prefix,unsigned depth,const std::filesystem::path& output,const Options& options,const std::string& boot) {
    if (depth>directory_depth_limit) {
        accumulator.limit("Directory depth limit reached under "+prefix);
        return;
    }
    std::vector<disc::Entry> children;
    try { children=disc.entries(directory); }
    catch (const std::exception& error) {
        accumulator.limit("Directory "+prefix+" could not be read: "+error.what());
        return;
    }
    for (const auto& child:children) {
        if (accumulator.inventory.units.size()>=unit_limit) return;
        const auto path=prefix.empty()?child.name:prefix+"/"+child.name;
        try {
            if (child.directory) walk(accumulator,disc,child,path,depth+1,output,options,boot);
            else consider_file(accumulator,disc,child,path,output,options,boot);
        } catch (const std::exception& error) {
            Unit unit; unit.id=path; unit.kind="unreadable"; unit.detail=error.what();
            accumulator.inventory.units.push_back(std::move(unit));
        }
    }
}
std::string stage_name(Stage stage) {
    switch (stage) {
    case Stage::discovered: return "discovered";
    case Stage::parsed: return "parsed";
    case Stage::analyzed: return "analyzed";
    case Stage::llvm_generated: return "llvm-generated";
    case Stage::machine_compiled: return "machine-compiled";
    }
    return "discovered";
}
}
Inventory convert_disc(const std::filesystem::path& image,const Options& options) {
    const auto started=std::chrono::steady_clock::now();
    if (options.jobs>64) throw std::invalid_argument("Compile jobs must not exceed 64");
    if (options.output.empty()) throw std::runtime_error("Inventory output directory is required");
    if (std::filesystem::exists(options.output)) throw std::runtime_error("Inventory output directory already exists");
    std::filesystem::create_directories(options.output);
    disc::Iso9660 disc(image);
    Accumulator accumulator;
    accumulator.inventory.volume=disc.volume();
    accumulator.limit("Static discovery does not recover dynamically loaded, encrypted, transformed or indirectly addressed code.");
    accumulator.limit("Unrecognized files and non-ELF archive members are not decoded as instructions. Bounded packed-image candidates require full container validation.");
    accumulator.limit("VU microprograms are recovered only from a framed upload packet. This disc walk does not scan data for VU instructions.");
    accumulator.limit("Linked and executed coverage are not produced. A machine-code object was not linked or run.");
    accumulator.limit("An export that matches a native service library is still converted when the module itself converts. The object is not linked.");
    std::string boot;
    try {
        const auto cnf=disc.read(disc.find("SYSTEM.CNF"),65536);
        boot=disc::boot_path(cnf);
        accumulator.inventory.boot_path=boot;
    } catch (const std::exception& error) {
        accumulator.limit(std::string("Boot executable was not identified: ")+error.what());
    }
    walk(accumulator,disc,disc.root(),{},0,options.output,options,boot);
    if (accumulator.unrecognized>unrecognized_unit_limit)
        accumulator.limit(std::to_string(accumulator.unrecognized-unrecognized_unit_limit)+
            " additional unrecognized files were counted only in the undiscovered group.");
    accumulator.finish();
    accumulator.inventory.generation_seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
    compile_inventory(accumulator.inventory,options);
    write_build_plan(accumulator.inventory,options.output);
    write_call_reports(accumulator.inventory,options.output);
    return std::move(accumulator.inventory);
}
std::string format_report(const Inventory& inventory) {
    std::size_t counts[5]{};
    for (const auto& unit:inventory.units) {
        const auto reached=static_cast<int>(unit.stage);
        for (int stage=0;stage<=reached;++stage) ++counts[stage];
    }
    std::ostringstream report;
    report<<"AnyPS2 conversion inventory\nVolume: "<<inventory.volume<<"\nBoot: "<<inventory.boot_path<<"\n"
          <<"discovered="<<counts[0]<<" parsed="<<counts[1]<<" analyzed="<<counts[2]
          <<" llvm_generated="<<counts[3]<<" machine_compiled="<<counts[4]
          <<" linked=0 executed=0\n"
          <<"generation_seconds="<<inventory.generation_seconds<<" compilation_seconds="<<inventory.compilation_seconds<<" jobs="<<inventory.jobs<<'\n'
          <<"Stage counts are cumulative. A compiled unit is also counted in every earlier stage.\n\nLimits:\n";
    for (const auto& limit:inventory.limits) report<<"- "<<limit<<'\n';
    report<<"\nUnits:\n";
    for (const auto& unit:inventory.units) {
        report<<unit.id<<" kind="<<unit.kind<<" stage="<<stage_name(unit.stage)
              <<" instructions="<<unit.instructions<<" declared_words="<<unit.declared_words;
        if (unit.analysis_base) report<<" analysis_base="<<hex32(unit.analysis_base);
        if (unit.overlaps_native_service) report<<" native_service_overlap=1";
        if (unit.native_replacement) report<<" native_replacement=1";
        if (!unit.detail.empty()) report<<" detail="<<unit.detail;
        if (!unit.artifact.empty()) report<<" objects="<<unit.compiled_objects<<'/'<<unit.modules.size()<<" artifact="<<unit.artifact.generic_string();
        report<<'\n';
    }
    report<<"\nMissing behavior by subsystem:\n";
    std::string subsystem;
    for (const auto& gap:inventory.gaps) {
        if (gap.subsystem!=subsystem) { subsystem=gap.subsystem; report<<"\n["<<subsystem<<"]\n"; }
        report<<"  "<<gap.key<<" count="<<gap.count;
        if (gap.first_address) report<<" first="<<hex32(gap.first_address);
        if (!gap.owners.empty()) report<<" owners="<<gap.owners;
        if (!gap.detail.empty()) report<<"\n    "<<gap.detail;
        report<<'\n';
    }
    report<<"\nNo unit in this report was linked or executed. Compilation of one module does not establish boot, a frame, or gameplay.\n";
    return report.str();
}
}
