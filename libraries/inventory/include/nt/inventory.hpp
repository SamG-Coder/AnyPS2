#pragma once
#include <nt/ee_codegen.hpp>
#include <nt/iop_ir.hpp>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <nt/iop_inspection.hpp>
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
    std::vector<std::filesystem::path> modules;
    std::size_t compiled_objects=0;
};
struct GroupedGap {
    std::string subsystem;
    std::string key;
    std::string detail;
    std::string owners;
    std::size_t count=0;
    std::uint32_t first_address=0;
};
enum class CallBinding { intrinsic, native_dispatch, compiled_export, registered_handler, unresolved, unknown };
struct CallProvider {
    std::string source;
    std::uint16_t version=0;
    bool convertible=false;
};
struct LibraryCall {
    std::string domain; // "iop" import table or "ee" static syscall candidate.
    std::string library;
    std::string source;
    std::uint32_t selector=0;
    std::uint16_t version=0;
    std::optional<std::uint32_t> address;
    std::size_t sites=1;
    CallBinding binding=CallBinding::unknown;
    bool reached=false;
    std::vector<CallProvider> providers;
};
struct Inventory {
    std::string volume;
    std::string boot_path;
    std::vector<Unit> units;
    std::vector<GroupedGap> gaps;
    std::vector<LibraryCall> calls;
    std::vector<std::string> limits;
    double generation_seconds=0,compilation_seconds=0;
    unsigned jobs=0;
};
struct Options {
    std::filesystem::path output;
    std::vector<ee_codegen::PayloadMapping> payloads;
    bool compile=true;
    bool generate=true; // False: analyze calls/coverage without emitting game code.
    unsigned jobs=0; // Auto: at most four concurrent LLVM processes.
    std::filesystem::path compiler="clang";
};
// Walk recognizable ELF, IRX, packed images and disc-module archives. One failure is recorded
// on that unit. Unrecognized bytes are not decoded. Nothing is linked or executed.
Inventory convert_disc(const std::filesystem::path& image,const Options& options);
std::string format_report(const Inventory& inventory);
std::string format_json(const Inventory& inventory,const std::filesystem::path& output);
// Reusable conversion/build plan. Unknown code remains a reported blocker;
// generated libraries are never automatically substituted for missing services.
void write_build_plan(const Inventory& inventory,const std::filesystem::path& output);
void compile_inventory(Inventory& inventory,const Options& options);
// Records all declared imports, including those not reached during discovery.
void record_iop_calls(Inventory& inventory,std::span<const elf::IopModule> modules,
    std::span<const std::string> sources,std::span<const iop_codegen::ir::ModuleDiagnosis> diagnoses);
void write_call_reports(const Inventory& inventory,const std::filesystem::path& output);
std::string format_neighborhood(const iop_codegen::ir::Neighborhood& report);
}
