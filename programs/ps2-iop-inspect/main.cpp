#include <nt/inventory.hpp>
#include <charconv>
#include <fstream>
#include <iostream>
#include <sstream>

namespace {
std::uint32_t number(std::string_view text,int base=16) {
    if (text.starts_with("0x")) { text.remove_prefix(2); base=16; }
    std::uint32_t value=0;
    const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value,base);
    if (parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size()) throw std::invalid_argument("Invalid numeric argument");
    return value;
}
}
int main(int argc,char** argv) {
    try {
        if (argc<6) throw std::invalid_argument(
            "Usage: ps2-iop-inspect <module.irx> <base-hex> <new-report.md> --anchor <hex> [--radius <hex>] [--capture <directory>]\n"
            "Or: ps2-iop-inspect --bundle <new-report.md> --capture <directory> [--radius <hex>] --modules <base> <module> ...\n"
            "Use --capture without --anchor to select an observed PC or return call site automatically.");
        const bool bundle=std::string_view(argv[1])=="--bundle";
        const std::filesystem::path output=bundle ? argv[2] : argv[3];
        std::vector<nt::elf::IopModule> modules;
        if (!bundle) modules.push_back(nt::elf::load_iop_module(nt::read_file(argv[1]),number(argv[2])));
        std::optional<std::uint32_t> anchor;
        std::uint32_t radius=0x1000;
        std::filesystem::path capture;
        for (int index=bundle ? 3 : 4;index<argc;index+=2) {
            const std::string_view option=argv[index];
            if (option=="--modules" && bundle) {
                if ((argc-index-1)%2 || argc-index-1>2048) throw std::invalid_argument("Expected bounded base/module pairs");
                for (int item=index+1;item<argc;item+=2) {
                    std::string_view base=argv[item];
                    if (base.starts_with("sealed@")) base.remove_prefix(7);
                    if (base.starts_with("romdrv@")) base.remove_prefix(7);
                    modules.push_back(nt::elf::load_iop_module(nt::read_file(argv[item+1]),number(base)));
                }
                break;
            }
            if (index+1>=argc) throw std::invalid_argument("Inspection option has no value");
            if (option=="--anchor") anchor=number(argv[index+1]);
            else if (option=="--radius") radius=number(argv[index+1]);
            else if (option=="--capture") capture=argv[index+1];
            else throw std::invalid_argument("Unknown inspection option");
        }
        std::map<std::string,std::uint32_t> observed;
        if (!capture.empty()) {
            const auto file=capture/"iop-cpu.txt";
            if (std::filesystem::file_size(file)>16384) throw std::invalid_argument("IOP observation is oversized");
            std::ifstream input(file); std::string line;
            while (std::getline(input,line)) {
                const auto separator=line.find('=');
                if (separator==std::string::npos) continue;
                const auto key=line.substr(0,separator);
                if (key=="pc" || key=="active" || key.starts_with("gpr")) observed[key]=number(std::string_view(line).substr(separator+1),10);
            }
            if (!observed.contains("active") || observed.at("active")!=1) throw std::invalid_argument("IOP observation has no active continuation");
        }
        const auto owner=[&](std::uint32_t address)->const nt::elf::IopModule* {
            const nt::elf::IopModule* selected=nullptr;
            for (const auto& module:modules) {
                const auto extent=module.info ? std::min(module.initialized_size,module.info->text_size) : module.initialized_size;
                if (address>=module.base && address-module.base<extent) {
                    if (selected) throw std::invalid_argument("Ambiguous overlapping inspection modules");
                    selected=&module;
                }
            }
            return selected;
        };
        if (!anchor && !observed.empty()) {
            const auto pc=observed.at("pc"),returned=observed.at("gpr31");
            const auto caller=returned>=8 ? returned-8 : returned;
            // In a full bundle, a stopped native import belongs to a provider;
            // its caller provides the useful game-code neighborhood.
            if (bundle && owner(caller)) anchor=caller;
            else if (owner(pc)) anchor=pc;
            else if (owner(caller)) anchor=caller;
        }
        if (!anchor) throw std::invalid_argument("Provide --anchor or an active --capture");
        const auto* module=owner(*anchor);
        if (!module) throw std::invalid_argument("Inspection anchor has no input module owner");
        const auto report=nt::iop_codegen::ir::inspect_neighborhood(*module,*anchor,radius);
        auto text=nt::inventory::format_neighborhood(report);
        text+="\nModule: "+(module->info ? module->info->name : std::string("unnamed"))+"; base="+nt::hex32(module->base)+"\n";
        if (!observed.empty()) {
            text+="\n## Observed boundary (one run, not static invariants)\n\n";
            for (const auto& [key,value]:observed) text+=key+"="+nt::hex32(value)+"\n";
        }
        if (std::filesystem::exists(output)) throw std::invalid_argument("Inspection report already exists");
        nt::write_file(output,{reinterpret_cast<const std::uint8_t*>(text.data()),text.size()});
        std::cout<<"Offline neighborhood: "<<report.calls.size()<<" candidate call sites, "<<report.function_candidates.size()
                 <<" function candidates, "<<report.unknown.size()<<" undecoded words\nReport: "<<output.string()<<'\n';
        std::cout<<"Module: "<<(module->info ? module->info->name : "unnamed")<<" base="<<nt::hex32(module->base)<<'\n';
    } catch (const std::exception& error) { std::cerr<<"ps2-iop-inspect: "<<error.what()<<'\n'; return 1; }
}
