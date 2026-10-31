#include <nt/ee_codegen.hpp>
#include "native_layout.hpp"
#include <nt/analysis.hpp>
#include <nt/ee_address.hpp>
#include <nt/ee_syscalls.hpp>
#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace nt::ee_codegen {
namespace {
struct Prepared {
    elf::Executable executable;
    analysis::Program discovered;
    std::vector<CodeRange> ranges;
    std::vector<CodePayload> payloads;
    Bytes image;
    std::vector<LayoutDeclaration> layouts;
    std::vector<ExecutableImage> images;
    std::set<std::uint32_t> registered_calls;
    std::size_t wrapper_candidates=0;
    std::uint32_t syscall_table=0;
};
void save(const std::filesystem::path& path,const std::string& text) {
    write_file(path,ByteView(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()));
}
Prepared prepare(ByteView bytes,std::span<const PayloadMapping> mappings) {
    auto executable=elf::parse(bytes);
    std::vector<CodeRange> ranges;
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
        if (segment.flags&1) ranges.push_back({segment.address,segment.memory_size});
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
    std::set<std::uint32_t> registered_calls;
    while (true) {
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
        for (const auto address:analysis::stored_code_pointers(bytes,executable,discovered)) {
            if (discovered.instructions.contains(address) ||
                std::find(continuations.begin(),continuations.end(),address)!=continuations.end()) continue;
            continuations.push_back(address); added=true;
        }
        for (const auto address:analysis::bounded_switch_entries(bytes,executable,discovered)) {
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
        for (const auto address:analysis::registered_code_pointers(bytes,executable,discovered)) {
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
        if (!added) break;
    }
    Prepared result;
    result.executable=std::move(executable);
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
}
void project(const std::filesystem::path& input,const std::filesystem::path& output,std::span<const PayloadMapping> mappings,std::span<const std::filesystem::path> images) {
    const auto bytes=read_file(input);
    auto prepared=prepare(bytes,mappings);
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
            auto extra=prepare(source,{});
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
            for (auto range:extra.ranges) { range.image=id; prepared.ranges.push_back(range); }
            prepared.layouts.insert(prepared.layouts.end(),extra.layouts.begin(),extra.layouts.end());
            prepared.images.push_back(snapshot(source,extra.executable,id));
        }
        for (const auto& [pc,instruction]:prepared.discovered.instructions) prepared.discovered.unresolved.erase(pc);
    }
    if (!prepared.discovered.unresolved.empty())
        throw std::runtime_error("EE discovery reached unmapped code at "+hex32(*prepared.discovered.unresolved.begin()));
    std::vector<r5900::Instruction> instructions;
    for (const auto& [pc,instruction]:prepared.discovered.instructions) instructions.push_back(instruction);
    std::ostringstream llvm;
    emit_llvm(llvm,instructions,prepared.ranges,prepared.payloads,prepared.layouts,prepared.images);
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
find_program(ANYPS2_CLANG NAMES clang REQUIRED)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated.obj"
  COMMAND "${ANYPS2_CLANG}" -target x86_64-pc-windows-msvc -Werror -Wno-override-module -O2 -c
    "${CMAKE_CURRENT_SOURCE_DIR}/generated.ll" -o "${CMAKE_CURRENT_BINARY_DIR}/generated.obj"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/generated.ll" VERBATIM)
add_executable(ps2-native "${ANYPS2_ROOT}/programs/ee-native-probe/main.cpp"
  "${ANYPS2_ROOT}/libraries/binary/src/reader.cpp" "${ANYPS2_ROOT}/libraries/ee_native/src/numeric.cpp"
  "${ANYPS2_ROOT}/libraries/fileio/src/fileio.cpp"
  "${ANYPS2_ROOT}/libraries/disc/src/disc_files.cpp" "${ANYPS2_ROOT}/libraries/disc/src/iso9660.cpp"
  "${ANYPS2_ROOT}/libraries/ee_native/src/io.cpp" "${ANYPS2_ROOT}/libraries/ee_native/src/vector.cpp" "${CMAKE_CURRENT_BINARY_DIR}/generated.obj")
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
    if (!std::filesystem::create_directories(output)) throw std::runtime_error("EE output directory already exists");
    save(output/"generated.ll",llvm.str());
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
         "Build with ps2-build, then run ps2-native <program.data> [operation-budget].\n"
         "Data contains private input material and must remain local. This is not game boot.\n");
}
EeAssessment assess(ByteView bytes,std::span<const PayloadMapping> mappings) {
    EeAssessment result;
    try {
        const auto prepared=prepare(bytes,mappings);
        (void)prepare_native_layout(prepared.layouts);
        result.instructions=prepared.discovered.instructions.size();
        result.wrapper_candidates=prepared.wrapper_candidates;
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
