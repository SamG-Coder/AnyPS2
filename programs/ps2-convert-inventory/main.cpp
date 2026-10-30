#include <nt/inventory.hpp>
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
    if (argc<3) {
        std::cerr<<"Usage: ps2-convert-inventory <disc.iso> <new-output-directory> [--payloads mapping-file] [--no-compile]\n"
                   "Inventories recognizable EE, IOP and archive members, converts each independently,\n"
                   "and writes report.txt. Unrecognized bytes are not decoded. Nothing is linked or executed.\n";
        return 2;
    }
    try {
        nt::inventory::Options options;
        options.output=argv[2];
        for (int index=3;index<argc;++index) {
            const std::string_view argument=argv[index];
            if (argument=="--no-compile") options.compile=false;
            else if (argument=="--payloads") {
                if (index+1==argc) throw std::runtime_error("Expected a payload mapping file");
                options.payloads=load_payloads(argv[++index]);
            } else throw std::runtime_error("Unknown inventory option");
        }
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
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"ps2-convert-inventory: "<<error.what()<<'\n';
        return 1;
    }
}
