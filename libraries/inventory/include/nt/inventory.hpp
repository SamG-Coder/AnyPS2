#pragma once
#include <nt/ee_codegen.hpp>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace nt::inventory {
// Highest stage this pass actually reached. Later stages are not implied.
enum class Stage { discovered, parsed, analyzed, llvm_generated, machine_compiled };
struct Unit {
    std::string id;
    std::string kind;
    Stage stage=Stage::discovered;
    std::size_t instructions=0;
    std::size_t declared_words=0;
    std::uint32_t analysis_base=0;
    bool overlaps_native_service=false;
    bool native_replacement=false;
    std::string detail;
    std::filesystem::path artifact;
};
struct GroupedGap {
    std::string subsystem;
    std::string key;
    std::string detail;
    std::string owners;
    std::size_t count=0;
    std::uint32_t first_address=0;
};
struct Inventory {
    std::string volume;
    std::string boot_path;
    std::vector<Unit> units;
    std::vector<GroupedGap> gaps;
    std::vector<std::string> limits;
};
struct Options {
    std::filesystem::path output;
    std::vector<ee_codegen::PayloadMapping> payloads;
    bool compile=true;
};
// Walk recognizable ELF, IRX and disc-module archives. One failure is recorded
// on that unit. Unrecognized bytes are not decoded. Nothing is linked or executed.
Inventory convert_disc(const std::filesystem::path& image,const Options& options);
std::string format_report(const Inventory& inventory);
}
