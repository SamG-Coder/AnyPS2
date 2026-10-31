#include <nt/ee_codegen.hpp>
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
void project(const std::filesystem::path& input,const std::filesystem::path& output,std::span<const PayloadMapping> mappings) {
    const auto bytes=read_file(input);
    const auto prepared=prepare(bytes,mappings);
    if (!prepared.discovered.unresolved.empty())
        throw std::runtime_error("EE discovery reached unmapped code at "+hex32(*prepared.discovered.unresolved.begin()));
    std::vector<r5900::Instruction> instructions;
    for (const auto& [pc,instruction]:prepared.discovered.instructions) instructions.push_back(instruction);
    std::ostringstream llvm;
    emit_llvm(llvm,instructions,prepared.ranges,prepared.payloads);
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
                (instruction.rd==0 || instruction.rd==2 || instruction.rd==3 || instruction.rd==5 || instruction.rd==6 || instruction.rd==10);
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
