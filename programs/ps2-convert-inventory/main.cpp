#include <nt/inventory.hpp>
#include <nt/process.hpp>
#include <charconv>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
std::uint32_t number(std::string_view text) {
    int base=10;
    if (text.starts_with("0x") || text.starts_with("0X")) { base=16; text.remove_prefix(2); }
    std::uint32_t value=0;
    const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value,base);
    if (parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size())
        throw std::runtime_error("Invalid payload mapping number");
    return value;
}
std::vector<nt::ee_codegen::PayloadMapping> load_payloads(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Cannot open payload mapping file");
    std::vector<nt::ee_codegen::PayloadMapping> payloads;
    std::string line;
    while (std::getline(input,line)) {
        if (line.size()>256 || payloads.size()>=64) throw std::runtime_error("Payload mapping limit exceeded");
        std::istringstream fields(line);
        std::string source,destination,size,extra;
        if (!(fields>>source)) continue;
        if (!(fields>>destination>>size)) throw std::runtime_error("Expected source destination size per mapping line");
        nt::ee_codegen::PayloadMapping mapping{number(source),number(destination),number(size),{}};
        while (fields>>extra) mapping.entry_offsets.push_back(number(extra));
        payloads.push_back(std::move(mapping));
    }
    return payloads;
}
}
int main(int argc,char** argv) {
    if (argc<2 || std::string_view(argv[1])=="--help") {
        std::cerr<<"Usage: ps2-convert-inventory <disc.iso> [new-output-directory] [--payloads mapping-file] [--no-compile] [--audit-only] [--jobs N] [--compiler path] [--build] [--source-root path]\n"
                   "Inventories recognizable EE, IOP and archive members, converts each independently,\n"
                   "and writes report.txt, library-calls.json/.md, build-plan.json and a CMake library/executable plan.\n"
                   "--audit-only collects calls and coverage without generating or compiling game code.\n"
                   "--build also links convertible targets. Unrecognized bytes are not decoded; no game is executed.\n";
        return argc<2 ? 2 : 0;
    }
    try {
        nt::inventory::Options options;
        options.output=std::filesystem::path("work")/(std::filesystem::path(argv[1]).stem().string()+"-native");
        int first=2;
        if (argc>2 && !std::string_view(argv[2]).starts_with("--")) options.output=argv[first++];
        bool build=false;
        std::filesystem::path source_root=ANYPS2_SOURCE_ROOT;
        for (int index=first;index<argc;++index) {
            const std::string_view argument=argv[index];
            if (argument=="--no-compile") options.compile=false;
            else if (argument=="--audit-only") { options.generate=false; options.compile=false; }
            else if (argument=="--build") build=true;
            else if (argument=="--jobs" || argument=="--compiler" || argument=="--source-root") {
                if (index+1==argc) throw std::runtime_error("Expected option value");
                const std::string value=argv[++index];
                if (argument=="--jobs") { options.jobs=number(value); if (!options.jobs || options.jobs>64) throw std::runtime_error("Jobs must be between 1 and 64"); }
                else if (argument=="--compiler") options.compiler=value;
                else source_root=value;
            }
            else if (argument=="--payloads") {
                if (index+1==argc) throw std::runtime_error("Expected a payload mapping file");
                options.payloads=load_payloads(argv[++index]);
            } else throw std::runtime_error("Unknown inventory option");
        }
        // The generated build compiles its own objects; avoid compiling twice.
        if (build && !options.generate) throw std::runtime_error("--audit-only cannot be combined with --build");
        if (build) options.compile=false;
        const auto inventory=nt::inventory::convert_disc(argv[1],options);
        const auto text=nt::inventory::format_report(inventory);
        const auto report=options.output/"report.txt";
        std::ofstream output(report);
        output<<text;
        if (!output) throw std::runtime_error("Cannot write inventory report");
        std::size_t generated=0,compiled=0;
        for (const auto& unit:inventory.units) {
            if (unit.stage==nt::inventory::Stage::llvm_generated || unit.stage==nt::inventory::Stage::machine_compiled) ++generated;
            if (unit.stage==nt::inventory::Stage::machine_compiled) ++compiled;
        }
        std::cout<<"Conversion inventory written to "<<report.string()
                 <<"\nunits="<<inventory.units.size()<<" llvm_generated="<<generated
                 <<" machine_compiled="<<compiled<<" grouped_gaps="<<inventory.gaps.size()
                 <<"\nLinked and executed coverage were not produced. This is not a game boot.\n";
        if (build) {
            const auto build_dir=options.output/"build";
            std::vector<std::string> configure{"cmake","-S",options.output.string(),"-B",build_dir.string(),
                "-DANYPS2_ROOT="+std::filesystem::absolute(source_root).generic_string()};
            if (options.compiler!="clang") configure.push_back("-DANYPS2_CLANG="+options.compiler.generic_string());
            auto status=nt::run_process(configure);
            if (status) return status;
            status=nt::run_process({"cmake","--build",build_dir.string(),"--config","Release","--parallel",std::to_string(inventory.jobs)});
            if (status) return status;
            std::ofstream receipt(options.output/"build-result.txt");
            const bool executable=std::filesystem::exists(options.output/"bin"/"ps2-native.exe");
            receipt<<"build_succeeded=true\nboot_executable="<<(executable ? "bin/ps2-native.exe" : "absent-conversion-blocked")
                   <<"\nexecuted=false\nLibraries are independent units; complete runtime dependency binding is not implied.\n";
            if (!receipt) throw std::runtime_error("Cannot record batch build result");
            std::cout<<"Batch build completed. Outputs: "<<(options.output/"libraries").string()
                     <<"; boot executable "<<(executable ? "built" : "blocked by conversion gaps")<<". Not executed.\n";
            if (!executable) return 3;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"ps2-convert-inventory: "<<error.what()<<'\n';
        return 1;
    }
}
