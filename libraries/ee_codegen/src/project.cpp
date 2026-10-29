#include <nt/ee_codegen.hpp>
#include <nt/analysis.hpp>
#include <nt/ee_syscalls.hpp>
#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace nt::ee_codegen {
namespace {
void save(const std::filesystem::path& path,const std::string& text) {
    write_file(path,ByteView(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()));
}
}
void project(const std::filesystem::path& input,const std::filesystem::path& output) {
    const auto bytes=read_file(input);
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
    std::vector<std::uint32_t> continuations;
    analysis::Program discovered;
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
            if (setup.rs || setup.rt!=3 || !ee::startup_syscall(static_cast<std::uint32_t>(setup.immediate)) ||
                (setup.op!=r5900::Op::addiu && setup.op!=r5900::Op::daddiu && setup.op!=r5900::Op::ori)) continue;
            if (std::find(continuations.begin(),continuations.end(),pc+4)!=continuations.end()) continue;
            continuations.push_back(pc+4); added=true;
        }
        if (!added) break;
    }
    if (!discovered.unresolved.empty())
        throw std::runtime_error("EE discovery reached unmapped code at "+hex32(*discovered.unresolved.begin()));
    std::vector<r5900::Instruction> instructions;
    for (const auto& [pc,instruction]:discovered.instructions) instructions.push_back(instruction);
    std::ostringstream llvm;
    emit_llvm(llvm,instructions,ranges);
    Bytes image(end,0);
    for (const auto& segment:executable.segments)
        std::copy_n(bytes.begin()+segment.offset,segment.file_size,image.begin()+segment.address);
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
  "${ANYPS2_ROOT}/libraries/ee_native/src/vector.cpp" "${CMAKE_CURRENT_BINARY_DIR}/generated.obj")
target_compile_features(ps2-native PRIVATE cxx_std_20)
target_include_directories(ps2-native PRIVATE "${ANYPS2_ROOT}/libraries/ee_codegen/include"
  "${ANYPS2_ROOT}/libraries/runtime/include" "${ANYPS2_ROOT}/libraries/binary/include")
)cmake";
    cmake<<"target_compile_definitions(ps2-native PRIVATE ANYPS2_EE_ENTRY="<<executable.entry
         <<"U ANYPS2_EE_DATA_SIZE="<<image.size()<<"U ANYPS2_EE_DATA_HASH="<<fingerprint(image)<<"ULL)\n";
    if (!std::filesystem::create_directories(output)) throw std::runtime_error("EE output directory already exists");
    save(output/"generated.ll",llvm.str());
    save(output/"CMakeLists.txt",cmake.str());
    write_file(output/"program.data",image);
    save(output/"conversion.txt","AnyPS2 EE LLVM entry-flow conversion\n"
         "Discovery follows recognized native startup continuations. Other syscalls stop; no EE/IOP link is supplied.\n"
         "Indirect destinations must already belong to the discovered code set; other targets stop.\n"
         "Build with ps2-build, then run ps2-native <program.data> [operation-budget].\n"
         "Data contains private input material and must remain local. This is not game boot.\n");
}
}
