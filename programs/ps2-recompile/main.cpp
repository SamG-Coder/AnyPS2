#include <nt/ee_codegen.hpp>
#include <iostream>
#include <charconv>
#include <fstream>
#include <sstream>
#include <stdexcept>
int main(int argc,char** argv) {
    if (argc<3) {
        std::cerr<<"Usage: ps2-recompile <boot.elf> <new-output-directory> [--verify-only] [--whole-code] [--function-symbols] [--payloads mapping-file] [--image secondary.elf ...] [--observation native-stop-directory ...] [--patch patch-file]\n"
                 <<"Use --whole-code to compile every declared EE code word without caller discovery.\n"; return 2;
    }
    try {
        std::vector<nt::ee_codegen::PayloadMapping> payloads;
        std::vector<std::filesystem::path> images,observations;
        std::vector<nt::ee_codegen::InstructionPatch> patches;
        bool payload_option=false,verify_only=false,function_symbols=false,whole_code=false;
        for (int index=3;index<argc;++index) {
            const std::string_view option=argv[index];
            if (option=="--whole-code") {
                if (whole_code) throw std::runtime_error("Duplicate --whole-code option");
                whole_code=true; continue;
            }
            if (option=="--function-symbols") {
                if (function_symbols) throw std::runtime_error("Duplicate --function-symbols option");
                function_symbols=true; continue;
            }
            if (option=="--verify-only") {
                if (verify_only) throw std::runtime_error("Duplicate --verify-only option");
                verify_only=true; continue;
            }
            if (++index==argc) throw std::runtime_error("Missing conversion option value");
            if (option=="--image") {
                if (images.size()==31) throw std::runtime_error("Too many secondary images");
                images.emplace_back(argv[index]); continue;
            }
            if (option=="--patch") {
                if (!patches.empty()) throw std::runtime_error("Duplicate --patch option");
                std::ifstream input(argv[index]);
                if (!input) throw std::runtime_error("Cannot open code patch file");
                std::string line;
                while (std::getline(input,line)) {
                    if (line.empty() || line[0]=='#') continue;
                    std::istringstream fields(line);
                    nt::ee_codegen::InstructionPatch patch;
                    std::string extra;
                    if (!(fields>>std::hex>>patch.address>>patch.expected>>patch.replacement) || (fields>>extra))
                        throw std::runtime_error("Expected hexadecimal address, expected word and new word per patch line");
                    patches.push_back(patch);
                }
                if (patches.empty()) throw std::runtime_error("Code patch file has no entries");
                continue;
            }
            if (option=="--observation") {
                if (observations.size()==64) throw std::runtime_error("Too many native observations");
                observations.emplace_back(argv[index]); continue;
            }
            if (option!="--payloads" || payload_option) throw std::runtime_error("Expected --whole-code, --verify-only, --function-symbols, one --payloads or repeated --image/--observation options");
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
                while (fields>>extra) {
                    if (!extra.starts_with("code=")) { mapping.entry_offsets.push_back(number(extra)); continue; }
                    const std::string_view region(extra.data()+5,extra.size()-5);
                    const auto colon=region.find(':');
                    if (colon==std::string_view::npos) throw std::runtime_error("Expected code=offset:size in payload classification");
                    mapping.code_ranges.push_back({number(region.substr(0,colon)),number(region.substr(colon+1))});
                }
                payloads.push_back(std::move(mapping));
            }
        }
        if (verify_only) {
            const auto result=nt::ee_codegen::verify_flow(argv[1],argv[2],payloads,images,observations,function_symbols,whole_code,patches);
            const bool selected_code_ready=result.instructions && result.lowering_supported && result.layout_supported && !result.unmapped_targets;
            std::cout<<(whole_code ? "Whole declared EE code inventory" : "Reached-flow verification")<<": "
                <<(whole_code ? (selected_code_ready ? "lowerable" : "conversion gaps") : (result.closed() ? "closed" : "incomplete"))
                <<"; unresolved indirect sites="<<result.unresolved_indirect
                <<"; unclassified pointer references="<<result.unclassified_references
                <<"; unproven record bindings="<<result.record_binding_candidates<<".\n"
                <<"Reports written. No LLVM emission, native compilation or game execution performed.\n";
            if (whole_code) return selected_code_ready ? 0 : 2;
            return result.closed() ? 0 : 2;
        }
        nt::ee_codegen::project(argv[1],argv[2],payloads,images,observations,function_symbols,whole_code,patches);
        std::cout<<"Generated EE LLVM project. Compile it with ps2-build. Game boot is not established.\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<"EE conversion: "<<error.what()<<'\n'; return 1; }
}
