#include <nt/iop_codegen.hpp>
#include <charconv>
#include <iostream>
#include <stdexcept>

namespace {
std::uint32_t parse_base(std::string_view value) {
    if (value.starts_with("0x")) value.remove_prefix(2);
    std::uint32_t base=0;
    const auto parsed=std::from_chars(value.data(),value.data()+value.size(),base,16);
    if (parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size()) throw std::runtime_error("Invalid relocation base");
    return base;
}
}
int main(int argc,char** argv) {
    try {
        std::filesystem::path output;
        if (argc>=5 && std::string_view(argv[1])=="--bundle" && (argc-3)%2==0) {
            output=argv[2];
            std::vector<nt::iop_codegen::ModuleSource> inputs;
            for (int at=3;at<argc;at+=2) {
                std::string_view base=argv[at];
                bool sealed=false;
                if (base.starts_with("sealed@")) { sealed=true; base.remove_prefix(7); }
                const bool native=base.starts_with("romdrv@");
                if (native) base.remove_prefix(7);
                if ((sealed && native) || base.starts_with("sealed@"))
                    throw std::runtime_error("sealed@ cannot be combined with romdrv@");
                inputs.push_back({argv[at+1],parse_base(base),native,sealed});
            }
            nt::iop_codegen::project_bundle(inputs,output);
        } else if (argc==4 && std::string_view(argv[1])!="--bundle") {
            output=argv[2];
            nt::iop_codegen::project(argv[1],output,parse_base(argv[3]));
        } else throw std::runtime_error("Usage: ps2-iop-recompile <module.irx> <new-output-directory> <base-hex>\n"
            "   or: ps2-iop-recompile --bundle <new-output-directory> <base-hex> <module.irx> [<base-hex> <module.irx> ...]\n"
            "Prefix a bundle base with romdrv@ to explicitly replace a compatible ROMDRV 2.x module with native archive services.\n"
            "Prefix a bundle base with sealed@ to compile that module for a later LoadStartModule without placing it in the startup image.\n"
            "sealed@ and romdrv@ cannot be combined. A single-file conversion is never sealed.");
        std::cout<<"Generated IOP LLVM project: "<<output.string()
            <<"\nConfigure with CMake and -DANYPS2_ROOT=<source directory>, then build iop-native.\n";
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
