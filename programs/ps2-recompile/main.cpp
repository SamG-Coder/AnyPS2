#include <nt/ee_codegen.hpp>
#include <iostream>
#include <charconv>
#include <fstream>
#include <sstream>
#include <stdexcept>
int main(int argc,char** argv) {
    if (argc<3) {
        std::cerr<<"Usage: ps2-recompile <boot.elf> <new-output-directory> [--verify-only] [--payloads mapping-file] [--image secondary.elf ...] [--observation native-stop-directory ...]\n"
                 <<"LLVM entry-flow conversion follows supported native syscall continuations.\n"; return 2;
    }
    try {
        std::vector<nt::ee_codegen::PayloadMapping> payloads;
        std::vector<std::filesystem::path> images,observations;
        bool payload_option=false,verify_only=false;
        for (int index=3;index<argc;++index) {
            const std::string_view option=argv[index];
            if (option=="--verify-only") {
                if (verify_only) throw std::runtime_error("Duplicate --verify-only option");
                verify_only=true; continue;
            }
            if (++index==argc) throw std::runtime_error("Missing conversion option value");
            if (option=="--image") {
                if (images.size()==31) throw std::runtime_error("Too many secondary images");
                images.emplace_back(argv[index]); continue;
            }
            if (option=="--observation") {
                if (observations.size()==64) throw std::runtime_error("Too many native observations");
                observations.emplace_back(argv[index]); continue;
            }
            if (option!="--payloads" || payload_option) throw std::runtime_error("Expected --verify-only, one --payloads or repeated --image/--observation options");
            payload_option=true;
            std::ifstream input(argv[index]);
            if (!input) throw std::runtime_error("Cannot open payload mapping file");
            std::string line;
            while (std::getline(input,line)) {
                if (line.size()>256 || payloads.size()>=64) throw std::runtime_error("Payload mapping limit exceeded");
                std::istringstream fields(line); std::string source,destination,size,extra;
                if (!(fields>>source)) continue;
                if (!(fields>>destination>>size)) throw std::runtime_error("Expected source destination size per mapping line");
                const auto number=[](std::string_view text) {
                    int base=10;
                    if (text.starts_with("0x")) { base=16; text.remove_prefix(2); }
                    std::uint32_t value=0;
                    const auto result=std::from_chars(text.data(),text.data()+text.size(),value,base);
                    if (result.ec!=std::errc{} || result.ptr!=text.data()+text.size()) throw std::runtime_error("Invalid payload mapping number");
                    return value;
                };
                nt::ee_codegen::PayloadMapping mapping{number(source),number(destination),number(size),{}};
                while (fields>>extra) mapping.entry_offsets.push_back(number(extra));
                payloads.push_back(std::move(mapping));
            }
        }
        if (verify_only) {
            const auto result=nt::ee_codegen::verify_flow(argv[1],argv[2],payloads,images,observations);
            std::cout<<"Reached-flow verification: "<<(result.closed() ? "closed" : "incomplete")
                <<"; unresolved indirect sites="<<result.unresolved_indirect
                <<"; unclassified pointer references="<<result.unclassified_references
                <<"; unproven record bindings="<<result.record_binding_candidates<<".\n"
                <<"Reports written. No LLVM emission, native compilation or game execution performed.\n";
            return result.closed() ? 0 : 2;
        }
        nt::ee_codegen::project(argv[1],argv[2],payloads,images,observations);
        std::cout<<"Generated EE LLVM project. Compile it with ps2-build. Game boot is not established.\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<"EE conversion: "<<error.what()<<'\n'; return 1; }
}
