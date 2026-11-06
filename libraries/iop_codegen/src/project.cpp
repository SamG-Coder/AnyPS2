#include <nt/iop_codegen.hpp>
#include <nt/staged_directory.hpp>
#include "program.hpp"
#include <sstream>
#include <algorithm>
#include <array>
#include <stdexcept>
#include <fstream>
#include <memory>

namespace nt::iop_codegen {
namespace {
std::string metadata_string(std::string_view text) {
    std::string result="\"";
    for (const unsigned char byte:text) {
        if (byte=='"' || byte=='\\') { result+='\\'; result+=static_cast<char>(byte); }
        else if (byte>=32 && byte<127) result+=static_cast<char>(byte);
        else {
            result+='\\';
            result+=static_cast<char>('0'+((byte>>6)&7));
            result+=static_cast<char>('0'+((byte>>3)&7));
            result+=static_cast<char>('0'+(byte&7));
        }
    }
    return result+'"';
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
void project_bundle(std::span<const ModuleSource> inputs,const std::filesystem::path& destination) {
    if (inputs.empty()) throw std::runtime_error("IOP conversion: empty module bundle");
    std::vector<elf::IopModule> modules;
    std::vector<std::uint64_t> source_hashes;
    for (const auto& input : inputs) {
        if (input.native_romdrv && input.sealed)
            throw std::runtime_error("IOP conversion: sealed@ cannot be combined with romdrv@");
        if (input.sealed && (input.base&255U))
            throw std::runtime_error("IOP conversion: sealed module base is not 256-byte aligned");
        const auto bytes=read_file(input.path);
        source_hashes.push_back(fingerprint(bytes));
        modules.push_back(elf::load_iop_module(bytes,input.base));
    }
    std::vector<elf::IopModule> compiled;
    bool any_sealed=false;
    for (std::size_t index=0;index<modules.size();++index) {
        if (inputs[index].native_romdrv) {
            const auto exports=elf::iop_exports(modules[index]);
            if (exports.size()!=1 || exports.front().name!="romdrv" || exports.front().version>>8!=2 ||
                exports.front().functions.size()!=6)
                throw std::runtime_error("Native archive replacement requires the ROMDRV 2.x six-entry ABI");
        } else compiled.push_back(modules[index]);
        if (inputs[index].sealed) any_sealed=true;
        // Native replacements reserve their original extents as well. Never
        // hide overlap by removing the executable from the discovery input.
        for (std::size_t other=0;other<index;++other)
            if (modules[index].base<std::uint64_t(modules[other].base)+modules[other].memory.size() &&
                modules[other].base<std::uint64_t(modules[index].base)+modules[index].memory.size())
                throw std::runtime_error("IOP conversion: overlapping bundle modules");
    }
    // Sealed extents stay out of the startup image. A sealed module inside that
    // image would sit in the reserved heap prefix and could not be held later.
    bool startup=false;
    std::uint32_t first=0x200000, initialized_end=0, memory_end=0;
    for (std::size_t index=0;index<modules.size();++index) if (!inputs[index].sealed) {
        startup=true;
        const auto& item=modules[index];
        first=std::min(first,item.base);
        initialized_end=std::max(initialized_end,item.base+item.initialized_size);
        memory_end=std::max(memory_end,item.base+static_cast<std::uint32_t>(item.memory.size()));
    }
    if (!startup) { first=0; initialized_end=0; memory_end=0; }
    const auto rounded=[](std::uint32_t size) { return (std::uint64_t(size)+255U)&~std::uint64_t{255}; };
    struct SealedSpan { std::uint32_t begin; std::uint64_t end; };
    std::vector<SealedSpan> sealed_spans;
    for (std::size_t index=0;index<modules.size();++index) if (inputs[index].sealed) {
        const auto begin=modules[index].base;
        const auto end=std::uint64_t(begin)+rounded(static_cast<std::uint32_t>(modules[index].memory.size()));
        if (startup && begin<memory_end && std::uint64_t(first)<end)
            throw std::runtime_error("IOP conversion: sealed module overlaps the startup image");
        for (const auto& other : sealed_spans)
            if (begin<other.end && std::uint64_t(other.begin)<end)
                throw std::runtime_error("IOP conversion: overlapping sealed modules");
        sealed_spans.push_back({begin,end});
    }
    const auto program=ir::discover_bundle(compiled);
    if (memory_end>0x1f0000) throw std::runtime_error("IOP conversion: bundle overlaps native invocation stack");
    std::uint32_t startup_entry=0;
    if (!inputs.front().sealed) startup_entry=modules.front().entry;
    else for (std::size_t index=0;index<modules.size();++index) if (!inputs[index].sealed) { startup_entry=modules[index].entry; break; }
    elf::IopModule module{first,startup_entry,Bytes(initialized_end-first),initialized_end-first,{}};
    for (std::size_t index=0;index<modules.size();++index) if (!inputs[index].native_romdrv && !inputs[index].sealed) {
        const auto& item=modules[index];
        std::copy_n(item.memory.begin(),item.initialized_size,module.memory.begin()+item.base-first);
    }
    std::ostringstream code;
    for (std::size_t index=0;index<modules.size();++index)
        if (inputs[index].sealed && !program.instructions.contains(modules[index].entry))
            throw std::runtime_error("IOP conversion: sealed entry is not a compiled instruction at "+hex32(modules[index].entry));
    code<<"#include <nt/iop_native.hpp>\n#include <nt/iop_native_modload.hpp>\nusing namespace nt::iop;\n";
    code<<"std::uint64_t nt_iop_generated_data_hash() { return "<<fingerprint(module.memory)<<"ULL; }\n";
    code<<"#include <vector>\nstd::vector<std::uint64_t> nt_iop_generated_source_hashes() { return {";
    for (std::size_t index=0;index<source_hashes.size();++index) if (!inputs[index].sealed) code<<source_hashes[index]<<"ULL,";
    code<<"}; }\n";
    Bytes sealed_image;
    const auto details=[&](const elf::IopModule& item) {
        code<<"{";
        if (item.info) code<<metadata_string(item.info->name)<<","<<item.info->version<<"U,"<<item.info->text_size<<"U,"
            <<item.info->data_size<<"U,"<<item.info->bss_size<<"U";
        code<<"}";
    };
    code<<"std::vector<nt::iop::SealedModule> nt_iop_generated_sealed() { return {";
    if (any_sealed) {
        code<<'\n';
        for (std::size_t index=0;index<modules.size();++index) if (inputs[index].sealed) {
            const auto& item=modules[index];
            const auto image=ByteView(item.memory).first(item.initialized_size);
            code<<"{"<<source_hashes[index]<<"ULL,"<<fingerprint(image)<<"ULL,"<<item.base<<"U,"<<item.entry<<"U,"
                <<item.initialized_size<<"U,"<<item.memory.size()<<"U,"<<(item.info ? item.info->global_pointer : 0)<<"U,";
            details(item); code<<"},\n";
            sealed_image.insert(sealed_image.end(),image.begin(),image.end());
        }
    }
    code<<"}; }\n";
    code<<"NativeModule nt_iop_generated_module() { return {"<<module.base<<"U,"<<module.entry<<"U,"
        <<module.initialized_size<<"U,"<<memory_end-first<<"U,"
        <<(module.info ? module.info->global_pointer : 0)<<"U}; }\n";
    code<<"#include <nt/iop_native_libraries.hpp>\nstd::vector<NativeLibrarySpec> nt_iop_generated_libraries() { return {\n";
    for (const auto& item : compiled) for (const auto& library : elf::iop_exports(item)) {
        code<<"{"<<library.address<<"U,"<<library.version<<"U,"<<metadata_string(library.name)<<",{";
        for (const auto target : library.functions) code<<target<<"U,";
        code<<"},"<<item.entry<<"U},\n";
    }
    code<<"}; }\n";
    code<<"std::vector<NativeModule> nt_iop_generated_entries() { return {\n";
    for (std::size_t index=0;index<modules.size();++index) if (!inputs[index].sealed) {
        const auto& item=modules[index];
        code<<"{"<<item.base<<"U,"<<item.entry<<"U,"<<item.initialized_size<<"U,"<<item.memory.size()<<"U,"
            <<(item.info ? item.info->global_pointer : 0)<<"U,NativeModuleKind::"
            <<(inputs[index].native_romdrv ? "archive" : "compiled")<<",";
        details(item); code<<"},\n";
    }
    code<<"}; }\n";
    code<<"std::vector<NativeImportSpec> nt_iop_generated_imports() { return {\n";
    for (const auto& import : program.library_imports)
        code<<"{"<<import.offset<<"U,"<<import.table_offset<<"U,"<<metadata_string(import.library)<<","<<import.version<<"U,"<<import.ordinal<<"U},\n";
    code<<"}; }\n";
    // Finish validation before creating output. Never overwrite a prior conversion.
    StagedDirectory staged(destination);
    const auto& output=staged.path();
    write_text_file(output/"module_metadata.cpp",code.str());
    std::ofstream llvm(output/"generated.ll",std::ios::binary);
    llvm.exceptions(std::ios::badbit|std::ios::failbit);
    std::vector<std::unique_ptr<std::ofstream>> streams;
    std::string manifest="generated.ll\n";
    detail::llvm(llvm,program,[&](std::size_t group)->std::ostream& {
        const auto name="iop-code-"+std::to_string(group)+".ll";
        auto stream=std::make_unique<std::ofstream>(output/name,std::ios::binary);
        stream->exceptions(std::ios::badbit|std::ios::failbit);
        manifest+=name+'\n'; streams.push_back(std::move(stream));
        return *streams.back();
    });
    llvm.close();
    for (auto& stream:streams) stream->close();
    write_text_file(output/"llvm-modules.txt",manifest);
    write_file(output/"module.data",ByteView(module.memory).first(module.initialized_size));
    if (any_sealed) write_file(output/"sealed.data",sealed_image);
    std::string cmake=R"cmake(cmake_minimum_required(VERSION 3.24)
project(AnyPS2ConvertedIop LANGUAGES CXX)
if(NOT ANYPS2_ROOT)
  message(FATAL_ERROR "Pass -DANYPS2_ROOT=<AnyPS2 source directory>")
endif()
add_executable(iop-native module_metadata.cpp "${ANYPS2_ROOT}/programs/iop-native-probe/main.cpp")
target_compile_features(iop-native PRIVATE cxx_std_20)
include("${ANYPS2_ROOT}/libraries/audio/native.cmake")
anyps2_add_audio(nt_audio)
include("${ANYPS2_ROOT}/libraries/usb/native.cmake")
anyps2_add_usb(nt_usb)
target_link_libraries(iop-native PRIVATE nt_audio nt_usb)
target_include_directories(iop-native PRIVATE "${ANYPS2_ROOT}/libraries/runtime/include" "${ANYPS2_ROOT}/libraries/iop/include"
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
include("${ANYPS2_ROOT}/cmake/llvm_objects.cmake")
anyps2_add_llvm_library(iop_code "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(iop-native PRIVATE iop_code)
target_compile_definitions(iop-native PRIVATE ANYPS2_IOP_LLVM=1)
)cmake";
    write_text_file(output/"CMakeLists.txt",cmake);
    std::ostringstream layout;
    layout<<"AnyPS2 explicit module startup order (base, entry, memory size)\n";
    for (std::size_t index=0;index<modules.size();++index) {
        const auto& item=modules[index];
        layout<<hex32(item.base)<<' '<<hex32(item.entry)<<' '<<hex32(static_cast<std::uint32_t>(item.memory.size()))
              <<(inputs[index].sealed ? " sealed" : inputs[index].native_romdrv ? " native-ROMDRV-archive" : " LLVM")<<'\n';
    }
    write_text_file(output/"modules.txt",layout.str());
    std::ostringstream diagnostics;
    for (const auto& item:compiled) for (const auto& library:elf::iop_exports(item))
        if (library.name=="libsd") diagnostics<<"Native LIBSD public ABI: LLVM entry stubs call the Windows audio owner. "
            "Initialization, sample storage, PCM block streams and consumption-driven callbacks are supported. "
            "Unsupported voice playback and effect modes stop explicitly.\n";
    for (const auto& item:compiled) for (const auto& library:elf::iop_exports(item))
        if (library.name=="usbd") diagnostics<<"Native USBD public ABI: LLVM entry stubs call the Windows USB descriptor owner. "
            "Startup, driver registration/probing, descriptor queries, private data and unregistration are supported; "
            "supported HID control and input transfers use native Windows handles. "
            "Unsupported transfers and topology changes stop explicitly.\n";
    for (const auto& item:compiled) for (const auto& library:elf::iop_exports(item))
        if (library.name=="dev9") diagnostics<<"Native DEV9 public ABI: no expansion network/storage provider is installed. "
            "Initialization returns MODULE_NO_RESIDENT_END without registering exports. "
            "Direct export calls stop explicitly; no hardware identity or successful I/O is fabricated.\n";
    for (const auto& item:compiled) for (const auto& library:elf::iop_exports(item))
        if (library.name=="mcman") diagnostics<<"Native MCMAN public ABI: LLVM entry stubs call native memory card files. "
            "Card transport and authentication are not converted. Raw page and block calls stop explicitly.\n";
    for (const auto& item:compiled) for (const auto& import:elf::iop_imports(item))
        if (import.library=="cdvdman" && import.ordinal==22)
            diagnostics<<"Unavailable native service: cdvdman ordinal 22 (console iLink identity) at "
                       <<hex32(item.base+import.offset)<<". The compiled call stops if reached; no identity is synthesized.\n";
    for (const auto pc : program.guarded_callback_sites)
        diagnostics<<"Guarded native callback at "<<hex32(pc)<<": "<<program.instructions.at(pc).targets.size()
                   <<" compiled address-taken candidates; other runtime targets stop as unmapped code.\n";
    for (const auto pc : program.open_indirect_sites)
        diagnostics<<"Open indirect at "<<hex32(pc)
                   <<": an unbounded fixed-slot store may call any instruction compiled into this program; every other address stops as unmapped code.\n";
    write_text_file(output/"conversion.txt",std::string("AnyPS2 standalone IOP native conversion\n"
        "Compile the emitted code with the supplied CMake project.\n"
        "module.data holds initialized program data; it is never decoded as instructions.\n"
        "Startup entries run in input order with shared native services; see modules.txt.\n"
        "This is not automatic disc boot orchestration.\n"
        "This initial converter supports a bounded integer/control-flow subset.\n"
        "Imports, native service bindings and complete game conversion are unfinished.\n"
        "Indirect calls/jumps without bounded AOT candidates are rejected; actual destinations are checked at runtime.\n"
        "Keep commercial converted code and data local and out of source control.\n")+diagnostics.str());
    staged.publish();
}
}
