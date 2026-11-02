#include <nt/ee_codegen.hpp>
#include "native_layout.hpp"
#include <nt/analysis.hpp>
#include <nt/ee_address.hpp>
#include <nt/ee_syscalls.hpp>
#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <fstream>
#include <chrono>
#include <memory>

namespace nt::ee_codegen {
namespace {
// Publish only complete build artifacts. Every temporary directory is created
// exclusively by this owner; an existing directory is never reused or removed.
class StagedProject {
public:
    StagedProject(const StagedProject&)=delete;
    StagedProject& operator=(const StagedProject&)=delete;
    explicit StagedProject(std::filesystem::path destination) : destination_(std::move(destination)) {
        if (std::filesystem::exists(destination_)) throw std::runtime_error("EE output directory already exists");
        const auto parent=destination_.parent_path().empty() ? std::filesystem::path(".") : destination_.parent_path();
        std::filesystem::create_directories(parent);
        const auto stamp=std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        for (unsigned attempt=0;attempt<64;++attempt) {
            const auto candidate=parent/(destination_.filename().string()+".pending-"+stamp+"-"+std::to_string(attempt));
            if (std::filesystem::create_directory(candidate)) { path_=candidate; return; }
        }
        throw std::runtime_error("Cannot reserve a native conversion output directory");
    }
    ~StagedProject() {
        if (!path_.empty()) { std::error_code error; std::filesystem::remove_all(path_,error); }
    }
    const std::filesystem::path& path() const { return path_; }
    void publish() {
        if (std::filesystem::exists(destination_)) throw std::runtime_error("EE output directory already exists");
        std::filesystem::rename(path_,destination_); path_.clear();
    }
private:
    std::filesystem::path destination_,path_;
};
struct Prepared {
    struct Pass { unsigned image=1; std::size_t index,instructions,new_roots,new_registrations; };
    elf::Executable executable;
    analysis::Program discovered;
    std::vector<CodeRange> ranges;
    std::vector<CodePayload> payloads;
    Bytes image;
    std::vector<LayoutDeclaration> layouts;
    std::vector<ExecutableImage> images;
    std::vector<analysis::CallSite> calls;
    std::vector<analysis::PointerReference> pointers;
    std::vector<analysis::CallbackFlowGap> flow_gaps;
    std::vector<Pass> passes;
    std::set<std::uint32_t> registered_calls;
    std::size_t wrapper_candidates=0;
    std::uint32_t syscall_table=0;
};
void save(const std::filesystem::path& path,const std::string& text) {
    write_file(path,ByteView(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()));
}
std::vector<CodeRange> declared_code_ranges(const elf::Executable& executable) {
    std::vector<CodeRange> result;
    if (executable.inferred_permissions) return result;
    auto data=executable.non_executable_sections;
    std::sort(data.begin(),data.end(),[](const auto& a,const auto& b) { return a.address<b.address; });
    for (const auto& segment:executable.segments) {
        if (!(segment.flags&1)) continue;
        // BSS and allocated data sections are writable application storage,
        // even when the enclosing LOAD segment also contains executable code.
        auto cursor=std::uint64_t(segment.address);
        const auto end=cursor+segment.file_size;
        for (const auto& section:data) {
            if (section.address>=end) break;
            const auto finish=std::uint64_t(section.address)+section.size;
            if (finish<=cursor) continue;
            if (section.address>cursor)
                result.push_back({static_cast<std::uint32_t>(cursor),static_cast<std::uint32_t>(section.address-cursor)});
            cursor=std::min(end,finish);
        }
        if (cursor<end) result.push_back({static_cast<std::uint32_t>(cursor),static_cast<std::uint32_t>(end-cursor)});
    }
    return result;
}
Prepared prepare(ByteView bytes,std::span<const PayloadMapping> mappings,bool diagnose_flow=false) {
    auto executable=elf::parse(bytes);
    auto ranges=declared_code_ranges(executable);
    std::vector<std::pair<std::uint32_t,std::uint32_t>> extents;
    std::uint32_t end=0;
    for (const auto& segment:executable.segments) {
        // Initial projects accept physical RAM ELF layouts; aliases in data
        // accesses remain supported by generated code.
        if (std::uint64_t(segment.address)+segment.memory_size>0x2000000)
            throw std::runtime_error("EE project requires ELF segments in physical RAM");
        if (!segment.memory_size) continue;
        const auto finish=segment.address+segment.memory_size;
        for (const auto& [first,last]:extents)
            if (segment.address<last && first<finish) throw std::runtime_error("EE project has overlapping ELF segments");
        extents.emplace_back(segment.address,finish); end=std::max(end,finish);
    }
    const auto initial_executable=executable;
    std::vector<CodePayload> payloads;
    std::vector<std::uint32_t> payload_entries;
    for (const auto& mapping:mappings) {
        const auto destination=runtime::default_ram_address(mapping.destination);
        const auto finish=std::uint64_t(destination)+mapping.size;
        if (!mapping.size || mapping.source%4 || destination%4 || mapping.size%4 || finish>0x2000000U)
            throw std::runtime_error("Invalid copied payload mapping");
        for (const auto& [first,last]:extents)
            if (destination<last && first<finish) throw std::runtime_error("Copied payload destination overlaps reserved input storage");
        const auto source=std::find_if(initial_executable.segments.begin(),initial_executable.segments.end(),[&](const auto& segment) {
            return mapping.source>=segment.address && std::uint64_t(mapping.source)+mapping.size<=std::uint64_t(segment.address)+segment.file_size;
        });
        if (source==initial_executable.segments.end()) throw std::runtime_error("Copied payload source is not file-backed ELF storage");
        const auto offset=source->offset+mapping.source-source->address;
        payloads.push_back({destination,std::vector<std::uint8_t>(bytes.begin()+offset,bytes.begin()+offset+mapping.size)});
        executable.segments.push_back({offset,destination,mapping.size,mapping.size,5,4});
        payload_entries.push_back(destination);
        for (const auto entry_offset:mapping.entry_offsets) {
            if (entry_offset%4 || entry_offset>=mapping.size) throw std::runtime_error("Payload entry offset is outside its extent or unaligned");
            payload_entries.push_back(destination+entry_offset);
        }
        extents.emplace_back(destination,static_cast<std::uint32_t>(finish));
    }
    // Original native service metadata. Reserve a gap outside every ELF extent,
    // including BSS; never overwrite application data to expose registrations.
    std::sort(extents.begin(),extents.end());
    std::uint32_t syscall_table=0x10000;
    for (const auto& [begin,finish]:extents) {
        if (std::uint64_t(syscall_table)+1024<=begin) break;
        if (syscall_table<finish) syscall_table=(finish+3U)&~3U;
    }
    if (std::uint64_t(syscall_table)+1024>0x2000000U)
        throw std::runtime_error("No native syscall metadata extent available");
    std::vector<std::uint32_t> continuations=payload_entries;
    const auto wrappers=analysis::syscall_wrappers(bytes,executable);
    continuations.insert(continuations.end(),wrappers.begin(),wrappers.end());
    analysis::Program discovered;
    std::vector<analysis::PointerReference> pointers;
    std::set<std::uint32_t> registered_calls;
    std::vector<Prepared::Pass> passes;
    while (true) {
        const auto previous_roots=continuations.size();
        pointers.clear();
        discovered=analysis::discover(bytes,executable,program_instruction_limit,continuations,true);
        bool added=false;
        for (const auto& [pc,instruction]:discovered.instructions) {
            if (instruction.op!=r5900::Op::syscall || pc<4 || pc>0xfffffffbU) continue;
            const auto previous=discovered.instructions.find(pc-4);
            if (previous==discovered.instructions.end()) continue;
            const auto& setup=previous->second;
            // This supplies compilation candidates only. Runtime dispatch still
            // validates the actual syscall number and all service arguments.
            if (setup.rs || setup.rt!=3 || (!ee::startup_syscall(static_cast<std::uint32_t>(setup.immediate)) &&
                !registered_calls.contains(static_cast<std::uint32_t>(setup.immediate))) ||
                (setup.op!=r5900::Op::addiu && setup.op!=r5900::Op::daddiu && setup.op!=r5900::Op::ori)) continue;
            if (std::find(continuations.begin(),continuations.end(),pc+4)!=continuations.end()) continue;
            continuations.push_back(pc+4); added=true;
        }
        const auto previous_calls=registered_calls.size();
        for (const auto address:analysis::syscall_callbacks(bytes,executable,discovered,0x74,&registered_calls)) {
            if (discovered.instructions.contains(address) ||
                std::find(continuations.begin(),continuations.end(),address)!=continuations.end()) continue;
            continuations.push_back(address); added=true;
        }
        for (const auto call:{0x0aU,0x10U,0x12U}) for (const auto address:analysis::syscall_callbacks(bytes,executable,discovered,call)) {
            if (discovered.instructions.contains(address) ||
                std::find(continuations.begin(),continuations.end(),address)!=continuations.end()) continue;
            continuations.push_back(address); added=true;
        }
        for (const auto address:analysis::thread_entries(bytes,executable,discovered)) {
            if (discovered.instructions.contains(address) ||
                std::find(continuations.begin(),continuations.end(),address)!=continuations.end()) continue;
            continuations.push_back(address); added=true;
        }
        for (const auto address:analysis::stored_code_pointers(bytes,executable,discovered,&pointers)) {
            if (discovered.instructions.contains(address) ||
                std::find(continuations.begin(),continuations.end(),address)!=continuations.end()) continue;
            continuations.push_back(address); added=true;
        }
        for (const auto address:analysis::bounded_switch_entries(bytes,executable,discovered)) {
            if (discovered.instructions.contains(address) ||
                std::find(continuations.begin(),continuations.end(),address)!=continuations.end()) continue;
            continuations.push_back(address); added=true;
        }
        for (const auto address:analysis::initial_indirect_entries(bytes,executable,discovered)) {
            if (discovered.instructions.contains(address) ||
                std::find(continuations.begin(),continuations.end(),address)!=continuations.end()) continue;
            continuations.push_back(address); added=true;
        }
        for (const auto address:analysis::referenced_lifecycle_entries(bytes,executable,discovered)) {
            if (discovered.instructions.contains(address) ||
                std::find(continuations.begin(),continuations.end(),address)!=continuations.end()) continue;
            continuations.push_back(address); added=true;
        }
        for (const auto address:analysis::object_table_entries(bytes,executable,discovered)) {
            if (discovered.instructions.contains(address) ||
                std::find(continuations.begin(),continuations.end(),address)!=continuations.end()) continue;
            continuations.push_back(address); added=true;
        }
        for (const auto address:analysis::registered_code_pointers(bytes,executable,discovered,&pointers)) {
            if (discovered.instructions.contains(address) ||
                std::find(continuations.begin(),continuations.end(),address)!=continuations.end()) continue;
            continuations.push_back(address); added=true;
        }
        if (!payloads.empty()) for (const auto address:analysis::materialized_code_pointers(bytes,executable,discovered)) {
            const auto payload=std::find_if(payloads.begin(),payloads.end(),[&](const auto& p) {
                return address>=p.address && std::uint64_t(address)<std::uint64_t(p.address)+p.bytes.size();
            });
            if (payload==payloads.end() || discovered.instructions.contains(address) ||
                std::find(continuations.begin(),continuations.end(),address)!=continuations.end()) continue;
            continuations.push_back(address); added=true;
        }
        added|=registered_calls.size()!=previous_calls;
        passes.push_back({1,passes.size()+1,discovered.instructions.size(),
            continuations.size()-previous_roots,registered_calls.size()-previous_calls});
        if (!added) break;
    }
    if (executable.inferred_permissions) for (const auto& [pc,instruction]:discovered.instructions) {
        if (!ranges.empty() && std::uint64_t(ranges.back().address)+ranges.back().size==pc) ranges.back().size+=4;
        else ranges.push_back({pc,4});
    }
    Prepared result;
    result.passes=std::move(passes);
    if (diagnose_flow) result.flow_gaps=analysis::callback_flow_gaps(bytes,executable,discovered,pointers);
    result.executable=std::move(executable);
    result.calls=analysis::call_sites(bytes,result.executable,discovered);
    result.pointers=std::move(pointers);
    for (const auto& record:analysis::layout_candidates(bytes,result.executable,discovered)) result.layouts.push_back(record.fields);
    result.discovered=std::move(discovered);
    result.ranges=std::move(ranges);
    result.payloads=std::move(payloads);
    result.syscall_table=syscall_table;
    result.registered_calls=std::move(registered_calls);
    result.wrapper_candidates=wrappers.size();
    result.image.assign(end,0);
    for (const auto& segment:initial_executable.segments)
        std::copy_n(bytes.begin()+segment.offset,segment.file_size,result.image.begin()+segment.address);
    return result;
}
Prepared prepare_project(const std::filesystem::path& input,std::span<const PayloadMapping> mappings,
                         std::span<const std::filesystem::path> images,bool diagnose_flow=false) {
    const auto bytes=read_file(input);
    auto prepared=prepare(bytes,mappings,diagnose_flow);
    if (images.size()>31) throw std::invalid_argument("Too many secondary executable images");
    if (!images.empty()) {
        const auto snapshot=[](ByteView source,const elf::Executable& executable,std::uint32_t id) {
            ExecutableImage image{executable.entry,id,{}};
            for (const auto& segment:executable.segments) if (segment.file_size)
                image.segments.push_back({segment.address,Bytes(source.begin()+segment.offset,source.begin()+segment.offset+segment.file_size)});
            return image;
        };
        for (auto& range:prepared.ranges) range.image=1;
        prepared.images.push_back(snapshot(bytes,elf::parse(bytes),1));
        auto occupied=prepared.executable.segments;
        for (const auto& path:images) {
            const auto source=read_file(path);
            auto extra=prepare(source,{},diagnose_flow);
            const auto id=static_cast<std::uint32_t>(prepared.images.size()+1);
            for (const auto& section:extra.executable.segments) {
                for (const auto& old:occupied)
                    if (std::uint64_t(section.address)<std::uint64_t(old.address)+old.memory_size &&
                        std::uint64_t(old.address)<std::uint64_t(section.address)+section.memory_size)
                        throw std::runtime_error("Executable image overlaps another image or resident payload");
                if (std::uint64_t(section.address)<std::uint64_t(prepared.syscall_table)+1024 &&
                    std::uint64_t(prepared.syscall_table)<std::uint64_t(section.address)+section.memory_size)
                    throw std::runtime_error("Executable image overlaps native service metadata");
            }
            occupied.insert(occupied.end(),extra.executable.segments.begin(),extra.executable.segments.end());
            for (const auto& [pc,instruction]:extra.discovered.instructions)
                if (!prepared.discovered.instructions.emplace(pc,instruction).second)
                    throw std::runtime_error("Executable image has an ambiguous compiled target");
            prepared.discovered.unresolved.insert(extra.discovered.unresolved.begin(),extra.discovered.unresolved.end());
            prepared.discovered.ordinary.insert(extra.discovered.ordinary.begin(),extra.discovered.ordinary.end());
            prepared.discovered.entries.insert(extra.discovered.entries.begin(),extra.discovered.entries.end());
            prepared.calls.insert(prepared.calls.end(),extra.calls.begin(),extra.calls.end());
            prepared.pointers.insert(prepared.pointers.end(),extra.pointers.begin(),extra.pointers.end());
            prepared.flow_gaps.insert(prepared.flow_gaps.end(),extra.flow_gaps.begin(),extra.flow_gaps.end());
            for (auto pass:extra.passes) { pass.image=id; prepared.passes.push_back(pass); }
            for (auto range:extra.ranges) { range.image=id; prepared.ranges.push_back(range); }
            prepared.layouts.insert(prepared.layouts.end(),extra.layouts.begin(),extra.layouts.end());
            prepared.images.push_back(snapshot(source,extra.executable,id));
        }
        for (const auto& [pc,instruction]:prepared.discovered.instructions) prepared.discovered.unresolved.erase(pc);
    }
    return prepared;
}
void write_discovery_reports(const std::filesystem::path& output,const Prepared& prepared,bool verification=false) {
    std::ostringstream calls;
    calls<<"pc\tkind\ttarget\tcoverage\n";
    for (const auto& call:prepared.calls) {
        calls<<hex32(call.pc)<<'\t'<<analysis::name(call.kind)<<'\t';
        if (call.target) calls<<hex32(*call.target);
        calls<<'\t';
        if (call.target) {
            if (!prepared.discovered.instructions.contains(*call.target)) calls<<"missing";
            else if (verification) calls<<(call.initial_data_candidate ? "discovered-initial-candidate" : "discovered");
            else calls<<(call.initial_data_candidate ? "compiled-initial-candidate" : "compiled");
        }
        else if (call.kind==analysis::CallKind::service) calls<<"native-service-boundary";
        else if (call.kind==analysis::CallKind::return_link || call.kind==analysis::CallKind::return_exception) calls<<"runtime-continuation";
        else calls<<"unresolved-indirect";
        calls<<'\n';
    }
    save(output/"call-sites.tsv",calls.str());
    std::ostringstream pointers;
    pointers<<"site\ttarget\tsource\tuse\tcoverage\tcallee\targument_index\n";
    for (const auto& reference:prepared.pointers) {
        pointers<<hex32(reference.site)<<'\t'<<hex32(reference.target)<<'\t'
                <<(reference.argument ? "argument" : "store")<<'\t'
                <<(reference.callable ? "indirect-call-consumer" : reference.data_read ? "data-read-observed" : "unclassified")<<'\t'
                <<(prepared.discovered.instructions.contains(reference.target) ? "discovered-EE-code" : "not-established-as-EE-code")<<'\t';
        if (reference.argument) pointers<<hex32(reference.callee)<<'\t'<<reference.argument_index;
        else pointers<<"-\t-";
        pointers<<'\n';
    }
    save(output/"pointer-references.tsv",pointers.str());
    std::ostringstream passes;
    passes<<"image\tpass\tdiscovered_instructions\tnew_roots\tnew_registrations\n";
    for (const auto& pass:prepared.passes)
        passes<<pass.image<<'\t'<<pass.index<<'\t'<<pass.instructions<<'\t'<<pass.new_roots<<'\t'<<pass.new_registrations<<'\n';
    save(output/"discovery-passes.tsv",passes.str());
}
}
void project(const std::filesystem::path& input,const std::filesystem::path& destination,std::span<const PayloadMapping> mappings,std::span<const std::filesystem::path> images) {
    auto prepared=prepare_project(input,mappings,images);
    if (!prepared.discovered.unresolved.empty())
        throw std::runtime_error("EE discovery reached unmapped code at "+hex32(*prepared.discovered.unresolved.begin()));
    std::vector<r5900::Instruction> instructions;
    for (const auto& [pc,instruction]:prepared.discovered.instructions) instructions.push_back(instruction);
    StagedProject staged(destination);
    const auto& output=staged.path();
    std::ofstream llvm(output/"generated.ll",std::ios::binary);
    llvm.exceptions(std::ios::badbit|std::ios::failbit);
    std::vector<std::string> modules{"generated.ll"};
    std::vector<std::unique_ptr<std::ofstream>> streams;
    emit_llvm(llvm,instructions,prepared.ranges,prepared.payloads,prepared.layouts,prepared.images,[&](std::size_t index)->std::ostream& {
        modules.push_back("generated-"+std::to_string(index)+".ll");
        auto stream=std::make_unique<std::ofstream>(output/modules.back(),std::ios::binary);
        stream->exceptions(std::ios::badbit|std::ios::failbit);
        streams.push_back(std::move(stream)); return *streams.back();
    });
    llvm.close();
    for (auto& stream:streams) stream->close();
    std::string module_manifest;
    for (const auto& name:modules) module_manifest+=name+'\n';
    save(output/"llvm-modules.txt",module_manifest);
    write_discovery_reports(output,prepared);
    const auto& executable=prepared.executable;
    const auto& image=prepared.image;
    const auto syscall_table=prepared.syscall_table;
    std::ostringstream cmake;
    cmake<<R"cmake(cmake_minimum_required(VERSION 3.24)
project(AnyPS2ConvertedEe LANGUAGES CXX)
if(NOT ANYPS2_ROOT)
  message(FATAL_ERROR "Pass -DANYPS2_ROOT=<AnyPS2 source directory>")
endif()
if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|amd64|x86_64")
  message(FATAL_ERROR "Initial EE LLVM output supports Windows x86-64 with MSVC")
endif()
include("${ANYPS2_ROOT}/cmake/llvm_objects.cmake")
anyps2_add_llvm_library(ee_code "${CMAKE_CURRENT_SOURCE_DIR}")
add_executable(ps2-native "${ANYPS2_ROOT}/programs/ee-native-probe/main.cpp"
  "${ANYPS2_ROOT}/libraries/binary/src/reader.cpp" "${ANYPS2_ROOT}/libraries/ee_native/src/numeric.cpp"
  "${ANYPS2_ROOT}/libraries/fileio/src/fileio.cpp"
  "${ANYPS2_ROOT}/libraries/disc/src/disc_files.cpp" "${ANYPS2_ROOT}/libraries/disc/src/iso9660.cpp"
  "${ANYPS2_ROOT}/libraries/ee_native/src/io.cpp" "${ANYPS2_ROOT}/libraries/ee_native/src/vector.cpp")
target_link_libraries(ps2-native PRIVATE ee_code)
add_custom_command(TARGET ps2-native POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/program.data" "$<TARGET_FILE_DIR:ps2-native>/program.data")
include("${ANYPS2_ROOT}/libraries/graphics/vulkan.cmake")
anyps2_add_vulkan(nt_vulkan)
include("${ANYPS2_ROOT}/libraries/host_metrics/native.cmake")
anyps2_add_host_metrics(nt_host_metrics)
target_link_libraries(ps2-native PRIVATE nt_vulkan nt_host_metrics)
include("${ANYPS2_ROOT}/libraries/vu/native.cmake")
anyps2_bind_vu(ps2-native)
include("${ANYPS2_ROOT}/libraries/iop/native.cmake")
anyps2_bind_iop(ps2-native)
include("${ANYPS2_ROOT}/cmake/native_output.cmake")
anyps2_check_native_output(ps2-native)
target_compile_features(ps2-native PRIVATE cxx_std_20)
target_include_directories(ps2-native PRIVATE "${ANYPS2_ROOT}/libraries/ee_codegen/include"
  "${ANYPS2_ROOT}/libraries/runtime/include" "${ANYPS2_ROOT}/libraries/binary/include"
  "${ANYPS2_ROOT}/libraries/disc/include" "${ANYPS2_ROOT}/libraries/fileio/include")
)cmake";
    cmake<<"target_compile_definitions(ps2-native PRIVATE ANYPS2_EE_ENTRY="<<executable.entry
         <<"U ANYPS2_EE_SYSCALL_TABLE="<<syscall_table<<"U ANYPS2_EE_DATA_SIZE="<<image.size()<<"U ANYPS2_EE_DATA_HASH="<<fingerprint(image)<<"ULL)\n";
    save(output/"CMakeLists.txt",cmake.str());
    std::ostringstream native_layout;
    for (const auto& record:prepared.layouts)
        native_layout<<hex32(record[0])<<' '<<hex32(record[1])<<' '<<hex32(record[2])<<' '<<hex32(record[3])<<'\n';
    save(output/"native-layout-candidates.txt",native_layout.str());
    if (!prepared.images.empty()) {
        std::ostringstream manifest;
        for (const auto& executable_image:prepared.images) {
            manifest<<"image "<<executable_image.id<<" entry "<<hex32(executable_image.entry)<<'\n';
            for (const auto& segment:executable_image.segments)
                manifest<<"  section "<<hex32(segment.address)<<" bytes "<<segment.bytes.size()<<'\n';
        }
        manifest<<"Only image 1 is initialized at launch. Other images must be loaded and validated at ExecPS2.\n";
        save(output/"executable-images.txt",manifest.str());
    }
    const auto layout_plan=prepare_native_layout(prepared.layouts);
    std::ostringstream accepted_layout;
    accepted_layout<<"Static native allocations exist from process startup; declarations validate the compiled plan.\n"
        "No mutable mappings, lookup tables, or address-space readback. Device bindings do not supply BIOS storage or device readiness.\n";
    for (const auto& view:layout_plan.aliases)
        accepted_layout<<"alias "<<hex32(view.address)<<" backing "<<hex32(view.backing)<<" bytes "<<hex32(view.bytes)<<'\n';
    for (const auto& guard:layout_plan.guards)
        accepted_layout<<"access "<<hex32(guard.address)<<" bytes "<<hex32(guard.bytes)
            <<" read "<<guard.readable<<" write "<<guard.writable<<'\n';
    accepted_layout<<"accepted declarations "<<layout_plan.declarations.size()<<'\n';
    save(output/"native-layout-plan.txt",accepted_layout.str());
    if (!mappings.empty()) {
        std::ostringstream plan;
        for (const auto& mapping:mappings) {
            plan<<mapping.source<<' '<<mapping.destination<<' '<<mapping.size;
            for (const auto offset:mapping.entry_offsets) plan<<' '<<offset;
            plan<<'\n';
        }
        save(output/"payloads.txt",plan.str());
    }
    write_file(output/"program.data",image);
    save(output/"conversion.txt","AnyPS2 EE LLVM entry-flow conversion\n"
         "Discovery includes leaf syscall-wrapper candidates and recognized startup continuations. Compiled wrappers do not implement missing services; runtime dispatch still validates them.\n"
         "Indirect destinations must already belong to the discovered code set; other targets stop.\n"
         "call-sites.tsv inventories reached transfers; compiled initial candidates do not prove complete indirect coverage.\n"
         "Build with ps2-build, then run ps2-native <program.data> [operation-budget].\n"
         "Data contains private input material and must remain local. This is not game boot.\n");
    staged.publish();
}
FlowVerification verify_flow(const std::filesystem::path& input,const std::filesystem::path& destination,
                             std::span<const PayloadMapping> mappings,std::span<const std::filesystem::path> images) {
    const auto prepared=prepare_project(input,mappings,images,true);
    FlowVerification result;
    result.instructions=prepared.discovered.instructions.size(); result.passes=prepared.passes.size();
    result.unmapped_targets=prepared.discovered.unresolved.size();
    result.record_binding_candidates=prepared.flow_gaps.size();
    StagedProject staged(destination);
    const auto& output=staged.path();
    write_discovery_reports(output,prepared,true);
    std::ostringstream gaps;
    gaps<<"kind\tsite\ttarget\tevidence\n";
    for (const auto target:prepared.discovered.unresolved)
        gaps<<"unmapped-target\t-\t"<<hex32(target)<<"\treached control flow has no mapped instruction\n";
    for (const auto& call:prepared.calls) {
        if (call.kind==analysis::CallKind::service) { ++result.service_boundaries; continue; }
        if (call.target || call.kind==analysis::CallKind::return_link || call.kind==analysis::CallKind::return_exception) continue;
        ++result.unresolved_indirect;
        gaps<<"unresolved-indirect\t"<<hex32(call.pc)<<"\t-\truntime target set is not proven complete\n";
    }
    for (const auto& reference:prepared.pointers) {
        if (reference.callable || prepared.discovered.instructions.contains(reference.target)) continue;
        ++result.unclassified_references;
        gaps<<"unclassified-pointer\t"<<hex32(reference.site)<<'\t'<<hex32(reference.target)<<'\t'
            <<(reference.argument ? "argument passed to " : "stored address; ");
        if (reference.argument) gaps<<hex32(reference.callee)<<" argument "<<reference.argument_index<<"; ";
        gaps<<(reference.data_read ? "data use observed, callable use not established" : "callable use not established")<<'\n';
    }
    save(output/"flow-gaps.tsv",gaps.str());
    std::ostringstream bindings;
    bindings<<"store\ttarget\tfield\tstatus\tconsumer_sites\n";
    for (const auto& gap:prepared.flow_gaps) {
        bindings<<hex32(gap.site)<<'\t'<<hex32(gap.target)<<'\t'<<hex32(gap.field)<<"\tunproven-object-identity\t";
        for (std::size_t n=0;n<gap.consumers.size();++n) {
            if (n) bindings<<',';
            bindings<<hex32(gap.consumers[n]);
        }
        bindings<<'\n';
    }
    save(output/"callback-flow-gaps.tsv",bindings.str());
    std::vector<r5900::Instruction> instructions;
    instructions.reserve(prepared.discovered.instructions.size());
    for (const auto& [pc,instruction]:prepared.discovered.instructions) instructions.push_back(instruction);
    const auto rejection=reject_unsupported(instructions);
    result.lowering_supported=!instructions.empty() && rejection.empty();
    save(output/"lowering-diagnostics.txt",rejection.text());
    std::string layout_error;
    try { (void)prepare_native_layout(prepared.layouts); result.layout_supported=true; }
    catch (const std::exception& error) { layout_error=error.what(); }
    save(output/"layout-diagnostics.txt",layout_error);
    std::ostringstream summary;
    summary<<"known_flow="<<(result.closed() ? "closed" : "incomplete")<<'\n'
        <<"discovered_instructions="<<result.instructions<<"\ndiscovery_passes="<<result.passes
        <<"\nunmapped_targets="<<result.unmapped_targets<<"\nunresolved_indirect="<<result.unresolved_indirect
        <<"\nunclassified_pointer_references="<<result.unclassified_references
        <<"\nunproven_record_bindings="<<result.record_binding_candidates<<"\nnative_service_boundaries="<<result.service_boundaries
        <<"\nlowering_supported="<<result.lowering_supported<<"\nlayout_supported="<<result.layout_supported
        <<"\nLLVM_emitted=0\nnative_compilation_performed=0\nruntime_executed=0\n"
        <<"Discovery closure means the implemented rules found no additional roots, not complete game coverage.\n"
        <<"Callback layout matches are diagnostics only and never promote a pointer to executable code.\n"
        <<"Unknown dynamic content, native service behavior, rendering and gameplay require separate validation.\n";
    save(output/"flow-verification.txt",summary.str());
    staged.publish();
    return result;
}
EeAssessment assess(ByteView bytes,std::span<const PayloadMapping> mappings) {
    EeAssessment result;
    try {
        const auto prepared=prepare(bytes,mappings);
        (void)prepare_native_layout(prepared.layouts);
        result.instructions=prepared.discovered.instructions.size();
        result.wrapper_candidates=prepared.wrapper_candidates;
        std::set<std::uint32_t> unclassified;
        for (const auto& reference:prepared.pointers)
            if (!reference.callable && !prepared.discovered.instructions.contains(reference.target)) unclassified.insert(reference.target);
        result.unclassified_pointer_count=unclassified.size();
        for (const auto target:unclassified) {
            if (result.unclassified_pointer_sample.size()==16) break;
            result.unclassified_pointer_sample.push_back(target);
        }
        result.registered_calls.assign(prepared.registered_calls.begin(),prepared.registered_calls.end());
        for (const auto& segment:prepared.executable.segments)
            if (segment.flags&1) result.declared_executable_words+=segment.memory_size/4;
        result.unresolved_count=prepared.discovered.unresolved.size();
        for (const auto address:prepared.discovered.unresolved) {
            if (result.unresolved_sample.size()==16) break;
            result.unresolved_sample.push_back(address);
        }
        std::vector<r5900::Instruction> instructions;
        instructions.reserve(prepared.discovered.instructions.size());
        for (const auto& [pc,instruction]:prepared.discovered.instructions) {
            instructions.push_back(instruction);
            using r5900::Op;
            const bool transfer=(instruction.op==Op::mtc0 || instruction.op==Op::mfc0) &&
                (instruction.rd==0 || instruction.rd==1 || instruction.rd==2 || instruction.rd==3 || instruction.rd==5 || instruction.rd==6 || instruction.rd==10);
            if (transfer || instruction.op==Op::tlbr || instruction.op==Op::tlbwi ||
                instruction.op==Op::tlbwr || instruction.op==Op::tlbp) {
                if (!result.address_space_requests) result.first_address_space_request=instruction.pc;
                ++result.address_space_requests;
            }
            if (!instruction.indirect()) continue;
            if (!result.indirect_sites) result.first_indirect=instruction.pc;
            ++result.indirect_sites;
        }
        for (const auto& instruction:instructions) {
            if (instruction.op!=r5900::Op::syscall) continue;
            const auto previous=prepared.discovered.instructions.find(instruction.pc-4);
            const auto& setup=previous==prepared.discovered.instructions.end() ? instruction : previous->second;
            if (previous==prepared.discovered.instructions.end() || setup.rs || setup.rt!=3 ||
                (setup.op!=r5900::Op::addiu && setup.op!=r5900::Op::daddiu && setup.op!=r5900::Op::ori)) {
                ++result.unknown_syscall_sites;
                continue;
            }
            ++result.known_syscalls[static_cast<std::uint32_t>(setup.immediate)];
        }
        if (!instructions.empty() && instructions.size()<=program_instruction_limit)
            result.rejection=reject_unsupported(instructions);
        result.convertible=result.unresolved_count==0 && result.rejection.empty() && !instructions.empty() &&
            instructions.size()<=program_instruction_limit;
    } catch (const std::exception& error) {
        result.layout_error=error.what();
        result.convertible=false;
    }
    return result;
}
}
