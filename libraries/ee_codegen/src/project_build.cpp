#include "project_internal.hpp"
#include <fstream>
#include <memory>
#include <sstream>

namespace nt::ee_codegen::project_detail {
void emit_project(const std::filesystem::path& output,const PreparedProject& prepared) {
    std::vector<r5900::Instruction> instructions;
    for (const auto& [pc,instruction]:prepared.discovered.instructions) instructions.push_back(instruction);
    std::ofstream llvm(output/"generated.ll",std::ios::binary);
    llvm.exceptions(std::ios::badbit|std::ios::failbit);
    std::vector<std::string> modules{"generated.ll"};
    std::vector<std::unique_ptr<std::ofstream>> streams;
    emit_llvm(llvm,instructions,prepared.ranges,prepared.payloads,prepared.layouts,prepared.images,[&](std::size_t index)->std::ostream& {
        modules.push_back("generated-"+std::to_string(index)+".ll");
        auto stream=std::make_unique<std::ofstream>(output/modules.back(),std::ios::binary);
        stream->exceptions(std::ios::badbit|std::ios::failbit);
        streams.push_back(std::move(stream)); return *streams.back();
    },prepared.input_identity,prepared.unverified_code);
    llvm.close();
    for (auto& stream:streams) stream->close();
    std::string module_manifest;
    for (const auto& name:modules) module_manifest+=name+'\n';
    write_text_file(output/"llvm-modules.txt",module_manifest);
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
include("${ANYPS2_ROOT}/libraries/input/native.cmake")
anyps2_add_input(nt_input)
include("${ANYPS2_ROOT}/libraries/audio/native.cmake")
anyps2_add_audio(nt_audio)
target_link_libraries(ps2-native PRIVATE nt_vulkan nt_host_metrics nt_input nt_audio)
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
    write_text_file(output/"CMakeLists.txt",cmake.str());
    write_file(output/"program.data",image);
}
}
