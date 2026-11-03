#include <nt/vu_codegen.hpp>
#include <charconv>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

int main(int argc,char** argv) {
    if (argc!=5 && argc!=6 && argc!=8) {
        std::cerr<<"Usage: ps2-vu-recompile <input.bin> <output.ll> <unit:0|1> <destination-index> [--all-entries | entry,entry,... [--continuations depth]]\n"
            <<"       ps2-vu-recompile --catalog <images.tsv> <output.ll> <unit:0|1>\n"; return 2;
    }
    try {
        const auto number=[](std::string_view text) {
            unsigned value=0; const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
            if (parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size()) throw std::invalid_argument("Expected decimal argument");
            return value;
        };
        if (std::string_view(argv[1])=="--catalog") {
            if (argc!=5) throw std::invalid_argument("Expected catalog, output and unit");
            if (std::filesystem::exists(argv[3])) throw std::invalid_argument("VU output already exists");
            const auto unit=number(argv[4]);
            const std::filesystem::path manifest=argv[2];
            const auto bytes=nt::read_file(manifest,1024*1024);
            std::istringstream rows(std::string(bytes.begin(),bytes.end()));
            std::vector<std::vector<nt::vu::Pair>> images;
            std::string row;
            while (std::getline(rows,row)) {
                if (!row.empty() && row.back()=='\r') row.pop_back();
                if (row.empty() || row=="file\tdestination_pairs") continue;
                const auto separator=row.find('\t');
                if (separator==std::string::npos || separator==0 || images.size()>=4096)
                    throw std::invalid_argument("Expected file and destination_pairs columns in VU catalog");
                const auto path=manifest.parent_path()/row.substr(0,separator);
                images.push_back(nt::vu::frame(nt::read_file(path,16384),unit,number(std::string_view(row).substr(separator+1))));
            }
            std::ostringstream module;
            nt::vu::emit_catalog_llvm(module,images,unit,unit ? "nt_vu_program1" : "nt_vu_program");
            const auto text=module.str();
            nt::write_text_file(argv[3], text);
            std::cout<<"Emitted "<<images.size()<<" whole-entry VU artifacts and native catalog. LLVM compilation and linking remain separate stages.\n";
            return 0;
        }
        if (std::filesystem::exists(argv[2])) throw std::invalid_argument("VU output already exists");
        const auto unit=number(argv[3]);
        const auto pairs=nt::vu::frame(nt::read_file(argv[1],16384),unit,number(argv[4]));
        std::ostringstream output;
        unsigned continuation_depth=0;
        if (argc==8) {
            if (std::string_view(argv[6])!="--continuations") throw std::invalid_argument("Expected --continuations depth");
            continuation_depth=number(argv[7]);
        }
        if (argc>=6 && std::string_view(argv[5])=="--all-entries") {
            if (argc!=6) throw std::invalid_argument("--all-entries does not accept continuation options");
            nt::vu::emit_program_llvm(output,pairs,unit,{},unit ? "nt_vu_program1" : "nt_vu_program",0,true);
            std::cout<<"LLVM entry coverage: "<<pairs.size()<<" of "<<pairs.size()<<" declared VU pairs. External targets remain explicit unmapped boundaries.\n";
        } else if (argc>=6) {
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
        nt::write_text_file(argv[2], text);
        std::cout<<"Emitted VU LLVM region; compilation and runtime binding are separate stages.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr<<"VU conversion: "<<error.what()<<'\n'; return 1; }
}
