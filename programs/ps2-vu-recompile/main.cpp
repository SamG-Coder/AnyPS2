#include <nt/vu_codegen.hpp>
#include <charconv>
#include <iostream>
#include <sstream>
#include <stdexcept>

int main(int argc,char** argv) {
    if (argc!=5 && argc!=6 && argc!=8) {
        std::cerr<<"Usage: ps2-vu-recompile <input.bin> <output.ll> <unit:0|1> <destination-index> [entry,entry,... [--continuations depth]]\n"; return 2;
    }
    try {
        const auto number=[](std::string_view text) {
            unsigned value=0; const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
            if (parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size()) throw std::invalid_argument("Expected decimal argument");
            return value;
        };
        if (std::filesystem::exists(argv[2])) throw std::invalid_argument("VU output already exists");
        const auto unit=number(argv[3]);
        const auto pairs=nt::vu::frame(nt::read_file(argv[1],16384),unit,number(argv[4]));
        std::ostringstream output;
        unsigned continuation_depth=0;
        if (argc==8) {
            if (std::string_view(argv[6])!="--continuations") throw std::invalid_argument("Expected --continuations depth");
            continuation_depth=number(argv[7]);
        }
        if (argc>=6) {
            std::vector<unsigned> entries;
            std::string_view list=argv[5];
            while (true) {
                const auto comma=list.find(',');
                entries.push_back(number(list.substr(0,comma)));
                if (comma==std::string_view::npos) break;
                list.remove_prefix(comma+1);
            }
            nt::vu::emit_program_llvm(output,pairs,unit,entries,unit ? "nt_vu_program1" : "nt_vu_program",continuation_depth);
        } else nt::vu::emit_llvm(output,pairs,unit);
        const auto text=output.str();
        nt::write_file(argv[2],nt::ByteView(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()));
        std::cout<<"Emitted VU LLVM region; compilation and runtime binding are separate stages.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr<<"VU conversion: "<<error.what()<<'\n'; return 1; }
}
