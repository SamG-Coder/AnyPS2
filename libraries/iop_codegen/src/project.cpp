#include <nt/iop_codegen.hpp>
#include "program.hpp"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <array>
#include <stdexcept>

namespace nt::iop_codegen {
namespace {
void save(const std::filesystem::path& file,const std::string& text) {
    write_file(file,ByteView(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()));
}
}
void emit_bundle(std::ostream& output,std::span<const elf::IopModule> modules,std::span<const std::uint32_t> entries) {
    const auto program=ir::discover_bundle(modules,entries);
    detail::llvm(output,program);
}
void project(const std::filesystem::path& input,const std::filesystem::path& output,std::uint32_t base) {
    const std::array inputs{ModuleSource{input,base}};
    project_bundle(inputs,output);
}
void project_bundle(std::span<const ModuleSource> inputs,const std::filesystem::path& output) {
    if (inputs.empty()) throw std::runtime_error("IOP conversion: empty module bundle");
    std::vector<elf::IopModule> modules;
    for (const auto& input : inputs) modules.push_back(elf::load_iop_module(read_file(input.path),input.base));
    std::vector<elf::IopModule> compiled;
    for (std::size_t index=0;index<modules.size();++index) {
        if (inputs[index].native_romdrv) {
            const auto exports=elf::iop_exports(modules[index]);
            if (exports.size()!=1 || exports.front().name!="romdrv" || exports.front().version>>8!=2 ||
                exports.front().functions.size()!=6)
                throw std::runtime_error("Native archive replacement requires the ROMDRV 2.x six-entry ABI");
        } else compiled.push_back(modules[index]);
        // Native replacements reserve their original extents as well. Never
        // hide overlap by removing the executable from the discovery input.
        for (std::size_t other=0;other<index;++other)
            if (modules[index].base<std::uint64_t(modules[other].base)+modules[other].memory.size() &&
                modules[other].base<std::uint64_t(modules[index].base)+modules[index].memory.size())
                throw std::runtime_error("IOP conversion: overlapping bundle modules");
    }
    const auto program=ir::discover_bundle(compiled);
    std::uint32_t first=0x200000, initialized_end=0, memory_end=0;
    for (const auto& item : modules) {
        first=std::min(first,item.base);
        initialized_end=std::max(initialized_end,item.base+item.initialized_size);
        memory_end=std::max(memory_end,item.base+static_cast<std::uint32_t>(item.memory.size()));
    }
    if (memory_end>0x1f0000) throw std::runtime_error("IOP conversion: bundle overlaps native invocation stack");
    elf::IopModule module{first,modules.front().entry,Bytes(initialized_end-first),initialized_end-first,{}};
    for (std::size_t index=0;index<modules.size();++index) if (!inputs[index].native_romdrv) {
        const auto& item=modules[index];
        std::copy_n(item.memory.begin(),item.initialized_size,module.memory.begin()+item.base-first);
    }
    std::ostringstream code, llvm;
    detail::llvm(llvm,program);
    code<<"#include <nt/iop_native.hpp>\nusing namespace nt::iop;\n";
    code<<"std::uint64_t nt_iop_generated_data_hash() { return "<<fingerprint(module.memory)<<"ULL; }\n";
    code<<"NativeModule nt_iop_generated_module() { return {"<<module.base<<"U,"<<module.entry<<"U,"
        <<module.initialized_size<<"U,"<<memory_end-first<<"U,"
        <<(module.info ? module.info->global_pointer : 0)<<"U}; }\n";
    code<<"#include <nt/iop_native_libraries.hpp>\nstd::vector<NativeLibrarySpec> nt_iop_generated_libraries() { return {\n";
    for (const auto& item : compiled) for (const auto& library : elf::iop_exports(item)) {
        code<<"{"<<library.address<<"U,"<<library.version<<"U,"<<std::quoted(library.name)<<",{";
        for (const auto target : library.functions) code<<target<<"U,";
        code<<"}},\n";
    }
    code<<"}; }\n";
    code<<"std::vector<NativeModule> nt_iop_generated_entries() { return {\n";
    for (std::size_t index=0;index<modules.size();++index) {
        const auto& item=modules[index];
        code<<"{"<<item.base<<"U,"<<item.entry<<"U,"<<item.initialized_size<<"U,"<<item.memory.size()<<"U,"
            <<(item.info ? item.info->global_pointer : 0)<<"U,NativeModuleKind::"
            <<(inputs[index].native_romdrv ? "archive" : "compiled")<<"},\n";
    }
    code<<"}; }\n";
    code<<"std::vector<NativeImportSpec> nt_iop_generated_imports() { return {\n";
    for (const auto& import : program.library_imports)
        code<<"{"<<import.offset<<"U,"<<import.table_offset<<"U,"<<std::quoted(import.library)<<","<<import.version<<"U,"<<import.ordinal<<"U},\n";
    code<<"}; }\n";
    // Finish validation before creating output. Never overwrite a prior conversion.
    if (!std::filesystem::create_directories(output)) throw std::runtime_error("IOP conversion: output directory already exists");
    save(output/"module_metadata.cpp",code.str());
    save(output/"generated.ll",llvm.str());
    write_file(output/"module.data",ByteView(module.memory).first(module.initialized_size));
    std::string cmake=R"cmake(cmake_minimum_required(VERSION 3.24)
project(AnyPS2ConvertedIop LANGUAGES CXX)
if(NOT ANYPS2_ROOT)
  message(FATAL_ERROR "Pass -DANYPS2_ROOT=<AnyPS2 source directory>")
endif()
add_executable(iop-native module_metadata.cpp "${ANYPS2_ROOT}/programs/iop-native-probe/main.cpp")
target_compile_features(iop-native PRIVATE cxx_std_20)
target_include_directories(iop-native PRIVATE "${ANYPS2_ROOT}/libraries/iop/include"
  "${ANYPS2_ROOT}/libraries/disc/include" "${ANYPS2_ROOT}/libraries/binary/include")
target_sources(iop-native PRIVATE "${ANYPS2_ROOT}/libraries/disc/src/disc_files.cpp"
  "${ANYPS2_ROOT}/libraries/disc/src/iso9660.cpp" "${ANYPS2_ROOT}/libraries/disc/src/reboot_image.cpp" "${ANYPS2_ROOT}/libraries/binary/src/reader.cpp")
# Only native data services: no instruction interpreter, decoder, JIT or devices.
add_custom_command(TARGET iop-native POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
  "${CMAKE_CURRENT_SOURCE_DIR}/module.data" "$<TARGET_FILE_DIR:iop-native>/module.data")
)cmake";
    cmake+=R"cmake(
if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|amd64|x86_64")
  message(FATAL_ERROR "Initial LLVM output currently supports Windows x86-64 with MSVC")
endif()
find_program(ANYPS2_CLANG NAMES clang REQUIRED)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated.obj"
  COMMAND "${ANYPS2_CLANG}" -target x86_64-pc-windows-msvc -Werror -Wno-override-module -O2 -c
    "${CMAKE_CURRENT_SOURCE_DIR}/generated.ll" -o "${CMAKE_CURRENT_BINARY_DIR}/generated.obj"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/generated.ll" VERBATIM)
target_sources(iop-native PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated.obj")
target_compile_definitions(iop-native PRIVATE ANYPS2_IOP_LLVM=1)
)cmake";
    save(output/"CMakeLists.txt",cmake);
    std::ostringstream layout;
    layout<<"AnyPS2 explicit module startup order (base, entry, memory size)\n";
    for (std::size_t index=0;index<modules.size();++index) {
        const auto& item=modules[index];
        layout<<hex32(item.base)<<' '<<hex32(item.entry)<<' '<<hex32(static_cast<std::uint32_t>(item.memory.size()))
              <<(inputs[index].native_romdrv ? " native-ROMDRV-archive" : " LLVM")<<'\n';
    }
    save(output/"modules.txt",layout.str());
    std::ostringstream diagnostics;
    for (const auto pc : program.guarded_callback_sites)
        diagnostics<<"Guarded stack callback at "<<hex32(pc)<<": "<<program.instructions.at(pc).targets.size()
                   <<" compiled address-taken candidates; other runtime targets stop as unmapped code.\n";
    save(output/"conversion.txt",std::string("AnyPS2 standalone IOP native conversion\n"
        "Compile the emitted code with the supplied CMake project.\n"
        "module.data holds initialized program data; it is never decoded as instructions.\n"
        "Startup entries run in input order with shared native services; see modules.txt.\n"
        "This is not automatic disc boot orchestration.\n"
        "This initial converter supports a bounded integer/control-flow subset.\n"
        "Imports, native service bindings and complete game conversion are unfinished.\n"
        "Indirect calls/jumps without bounded AOT candidates are rejected; actual destinations are checked at runtime.\n"
        "Keep commercial converted code and data local and out of source control.\n")+diagnostics.str());
}
}
