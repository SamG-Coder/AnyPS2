#include <nt/codegen.hpp>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace nt::codegen {
namespace {
void save(const std::filesystem::path& path, const std::string& text) {
    write_file(path, ByteView(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}
}
void project(const std::filesystem::path& input, const std::filesystem::path& output,
             std::span<const std::uint32_t> additional_entries, const std::filesystem::path& capture) {
    const auto bytes = read_file(input);
    const auto elf = elf::parse(bytes);
    auto program = analysis::discover(bytes, elf, 100000, capture.empty() ? additional_entries : std::span<const std::uint32_t>{});
    if (!capture.empty()) {
        std::ifstream identity_file(capture / "image-id.txt");
        std::uint64_t identity = 0;
        if (!(identity_file >> identity) || identity != fingerprint(bytes)) throw std::runtime_error("RAM capture belongs to a different ELF");
        const auto ram = read_file(capture / "ram.bin",32 * 1024 * 1024);
        if (ram.size() != 32 * 1024 * 1024 || additional_entries.empty()) throw std::runtime_error("Capture requires 32 MiB RAM and at least one observed entry");
        elf::Executable captured{additional_entries.front(),0,{
            {0,0,0x2000000,0x2000000,5,4},
            {0,0x80000000,0x2000000,0x2000000,5,4},
            {0,0xa0000000,0x2000000,0x2000000,5,4}}};
        const auto discovered = analysis::discover(ram,captured,100000,additional_entries);
        for (const auto& [pc,instruction] : discovered.instructions) {
            const auto [existing,inserted] = program.instructions.emplace(pc,instruction);
            if (!inserted && existing->second.word != instruction.word)
                throw std::runtime_error("Conflicting code versions require multi-version translation");
        }
        program.unresolved.insert(discovered.unresolved.begin(),discovered.unresolved.end());
        if (program.instructions.size() > 100000) throw std::runtime_error("Combined analysis limit exceeded");
    }
    std::ostringstream code, report;
    emit(code, program);
    code << "std::uint64_t nt_generated_identity() { return " << fingerprint(bytes) << "ULL; }\n";
    code << "std::uint32_t nt_generated_entry() { return " << hex32(elf.entry) << "U; }\n";
    report << "AnyPS2 diagnostic AOT project v1\n"
              "No PCSX2 devices or BIOS are linked. Only implemented HLE kernel calls are serviced.\n"
              "Indirect destinations are not inferred. This is not a playable game build.\n";
    analysis::describe(report, program);
    for (auto pc : additional_entries) report << "Additional entry: " << hex32(pc) << '\n';
    if (!capture.empty()) report << "Observed RAM source: " << std::filesystem::absolute(capture).generic_string() << '\n';
    if (!std::filesystem::create_directories(output)) throw std::runtime_error("Output directory already exists");
    save(output / "generated.cpp", code.str());
    save(output / "analysis.txt", report.str());
    write_file(output / "boot.elf", bytes);
    save(output / "CMakeLists.txt", R"cmake(cmake_minimum_required(VERSION 3.24)
project(AnyPS2Generated LANGUAGES CXX)
if(NOT ANYPS2_ROOT)
  message(FATAL_ERROR "Pass -DANYPS2_ROOT=<AnyPS2 source directory>")
endif()
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory("${ANYPS2_ROOT}" toolchain EXCLUDE_FROM_ALL)
add_executable(ps2-native generated.cpp "${ANYPS2_ROOT}/programs/native-probe/main.cpp")
target_compile_features(ps2-native PRIVATE cxx_std_20)
target_link_libraries(ps2-native PRIVATE nt_kernel nt_hardware nt_elf)
add_custom_command(TARGET ps2-native POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
  "${CMAKE_CURRENT_SOURCE_DIR}/boot.elf" "$<TARGET_FILE_DIR:ps2-native>/boot.elf")
)cmake");
}
}
