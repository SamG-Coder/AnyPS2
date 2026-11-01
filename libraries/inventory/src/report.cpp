#include <nt/inventory.hpp>
#include <nt/binary.hpp>
#include "report_text.hpp"
#include <sstream>

namespace nt::inventory {
namespace {
using detail::quote;
using detail::save;
std::string relative_path_text(const std::filesystem::path& path,const std::filesystem::path& root) {
    return path.empty() ? std::string{} : path.lexically_relative(root).generic_string();
}
std::string role(const Unit& unit) {
    if (unit.kind=="ee-boot") return "executable";
    if (unit.kind=="iop-module" || unit.kind=="ee-elf" || unit.kind=="ee-packed" || unit.kind.starts_with("vu")) return "library";
    return "input";
}
}
std::string format_json(const Inventory& inventory,const std::filesystem::path& output) {
    constexpr const char* stages[]{"discovered","parsed","analyzed","llvm-generated","machine-compiled"};
    std::ostringstream out;
    out<<"{\n  \"schema\":\"anyps2-build-plan-v1\",\n  \"volume\":"<<quote(inventory.volume)
       <<",\n  \"boot\":"<<quote(inventory.boot_path)<<",\n  \"generation_seconds\":"<<inventory.generation_seconds
       <<",\n  \"compilation_seconds\":"<<inventory.compilation_seconds<<",\n  \"jobs\":"<<inventory.jobs
       <<",\n  \"linked\":false,\n  \"executed\":false,\n  \"targets\":[\n";
    for (std::size_t index=0;index<inventory.units.size();++index) {
        const auto& unit=inventory.units[index];
        if (index) out<<",\n";
        const auto target=role(unit);
        out<<"    {\"source\":"<<quote(unit.id)<<",\"kind\":"<<quote(unit.kind)<<",\"role\":"<<quote(target)
           <<",\"stage\":"<<quote(stages[static_cast<unsigned>(unit.stage)])<<",\"detail\":"<<quote(unit.detail)
           <<",\"instructions\":"<<unit.instructions<<",\"declared_words\":"<<unit.declared_words
           <<",\"analysis_base\":"<<unit.analysis_base<<",\"compiled_objects\":"<<unit.compiled_objects
           <<",\"output\":"<<quote(target=="executable" ? "bin/ps2-native.exe" : target=="library" ?
               "libraries/unit_"+std::to_string(index)+".lib" : "")<<",\"modules\":[";
        for (std::size_t n=0;n<unit.modules.size();++n) {
            if (n) out<<',';
            out<<quote(relative_path_text(unit.modules[n],output));
        }
        out<<"]}";
    }
    out<<"\n  ],\n  \"missing\":[\n";
    for (std::size_t index=0;index<inventory.gaps.size();++index) {
        const auto& gap=inventory.gaps[index];
        if (index) out<<",\n";
        out<<"    {\"subsystem\":"<<quote(gap.subsystem)<<",\"key\":"<<quote(gap.key)
           <<",\"count\":"<<gap.count<<",\"first_address\":"<<gap.first_address
           <<",\"owners\":"<<quote(gap.owners)<<",\"detail\":"<<quote(gap.detail)<<'}';
    }
    out<<"\n  ],\n  \"limits\":[";
    for (std::size_t index=0;index<inventory.limits.size();++index) {
        if (index) out<<',';
        out<<quote(inventory.limits[index]);
    }
    out<<"]\n}\n";
    return out.str();
}
void write_build_plan(const Inventory& inventory,const std::filesystem::path& output) {
    save(output/"build-plan.json",format_json(inventory,output));
    std::ostringstream cmake;
    cmake<<"cmake_minimum_required(VERSION 3.24)\nproject(AnyPS2Batch LANGUAGES CXX)\n"
        "if(NOT ANYPS2_ROOT)\n  message(FATAL_ERROR \"Set ANYPS2_ROOT to the AnyPS2 source directory\")\nendif()\n"
        "include(\"${ANYPS2_ROOT}/cmake/llvm_objects.cmake\")\n"
        "set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY \"${CMAKE_SOURCE_DIR}/libraries\")\n"
        "set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE \"${CMAKE_SOURCE_DIR}/libraries\")\n"
        "set(CMAKE_RUNTIME_OUTPUT_DIRECTORY \"${CMAKE_SOURCE_DIR}/bin\")\n"
        "set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE \"${CMAKE_SOURCE_DIR}/bin\")\n";
    bool boot=false;
    for (std::size_t index=0;index<inventory.units.size();++index) {
        const auto& unit=inventory.units[index];
        if (unit.stage<Stage::llvm_generated || unit.modules.empty()) continue;
        const auto directory=relative_path_text(unit.artifact.parent_path(),output);
        if (role(unit)=="executable") {
            if (boot) throw std::runtime_error("Multiple boot executable candidates in batch plan");
            cmake<<"add_subdirectory(\""<<directory<<"\" boot-project)\n";
            boot=true;
        } else cmake<<"anyps2_add_llvm_library(unit_"<<index<<" \"${CMAKE_SOURCE_DIR}/"<<directory<<"\")\n";
    }
    cmake<<"message(STATUS \"Batch libraries are independent conversion artifacts; runtime dependencies still require validated binding.\")\n";
    if (!boot) cmake<<"message(STATUS \"Boot executable has unresolved conversion blockers; see build-plan.json.\")\n";
    save(output/"CMakeLists.txt",cmake.str());
}
}
