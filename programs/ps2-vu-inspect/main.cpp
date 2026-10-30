#include <nt/vu_program.hpp>
#include <nt/vu_arithmetic.hpp>
#include <nt/vu_lower.hpp>
#include <charconv>
#include <iostream>
#include <map>
#include <string_view>
#include <tuple>
#include <array>
#include <stdexcept>

int main(int argc,char** argv) {
    if (argc!=4) {
        std::cerr<<"Usage: ps2-vu-inspect <private-upload.bin> <unit:0|1> <destination-index>\n"; return 2;
    }
    try {
        const auto number=[](std::string_view text) {
            unsigned value=0; const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
            if (parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size()) throw std::invalid_argument("Expected decimal argument");
            return value;
        };
        const auto bytes=nt::read_file(argv[1],16384);
        const auto pairs=nt::vu::frame(bytes,number(argv[2]),number(argv[3]));
        using Key=std::tuple<bool,unsigned,unsigned,unsigned>;
        std::map<Key,unsigned> families;
        std::map<nt::vu::Arithmetic,unsigned> arithmetic;
        std::map<nt::vu::LowerOperation,unsigned> lower_operations;
        unsigned literals=0,ends=0;
        for (const auto& pair:pairs) {
            ++lower_operations[nt::vu::decode_lower(pair,number(argv[2])).operation];
            ++arithmetic[nt::vu::decode_upper(pair.upper).operation];
            const auto function=pair.upper.function;
            ++families[{true,0,function,function>=60 ? pair.upper.subfunction : 0}];
            if (pair.immediate) ++literals;
            if (pair.end) ++ends;
            if (pair.lower) {
                const auto& lower=*pair.lower;
                const bool special=lower.primary==64;
                ++families[{false,lower.primary,special ? lower.function : 0,
                    special && lower.function>=60 ? lower.subfunction : 0}];
            }
        }
        std::cout<<"pairs="<<pairs.size()<<" immediate_pairs="<<literals<<" end_flags="<<ends<<'\n';
        for (const auto& [key,count]:families) {
            const auto [upper,primary,function,subfunction]=key;
            std::cout<<(upper ? "upper" : "lower")<<" primary="<<primary<<" function="<<function
                     <<" subfunction="<<subfunction<<" count="<<count<<'\n';
        }
        for (const auto& [operation,count]:arithmetic) std::cout<<"arithmetic="<<nt::vu::name(operation)<<" count="<<count<<'\n';
        for (const auto& [operation,count]:lower_operations) std::cout<<"lower_operation="<<nt::vu::name(operation)<<" count="<<count<<'\n';
        const std::array entries{number(argv[3])};
        const auto flow=nt::vu::discover(pairs,number(argv[2]),entries);
        std::cout<<"entry_reachable="<<flow.reachable.size()<<" transfers="<<flow.transfers.size()
                 <<" missing_targets="<<flow.missing.size()<<" indirect_transfers="<<flow.indirect.size()
                 <<" complex_delays="<<flow.complex_delay.size()<<'\n';
        std::cout<<"Conversion inventory only; no LLVM generation or execution performed.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr<<"VU inspection: "<<error.what()<<'\n'; return 1; }
}
