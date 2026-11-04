#include <nt/packed_image.hpp>
#include <charconv>
#include <sstream>
#include <stdexcept>

namespace nt::package {
std::vector<Region> parse_layout(ByteView text,std::uint64_t input_identity) {
    if (text.size()>1024*1024) throw std::invalid_argument("Packed layout exceeds size bound");
    const auto number=[](std::string_view value) {
        int base=10;
        if (value.starts_with("0x")) { value.remove_prefix(2); base=16; }
        std::uint64_t result=0;
        const auto parsed=std::from_chars(value.data(),value.data()+value.size(),result,base);
        if (parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size())
            throw std::invalid_argument("Invalid packed layout number");
        return result;
    };
    std::istringstream input(std::string(text.begin(),text.end()));
    std::string version,identity;
    if (!(input>>version>>identity) || version!="anyps2-packed-layout-v1" || number(identity)!=input_identity)
        throw std::invalid_argument("Packed layout input identity differs or header is invalid");
    std::vector<Region> result;
    std::string kind,address,size;
    while (input>>kind) {
        if (!(input>>address>>size) || result.size()>=4094)
            throw std::invalid_argument("Invalid packed layout record or region limit");
        const auto at=number(address),length=number(size);
        if (at>0xffffffffULL || !length || length>0xffffffffULL || at+length>0x100000000ULL)
            throw std::invalid_argument("Packed layout region exceeds address space");
        const auto content=kind=="ee" ? Content::ee : kind=="data" ? Content::data : Content::vu;
        if (kind!="ee" && kind!="data" && kind!="vu") throw std::invalid_argument("Unknown packed layout content kind");
        result.push_back({static_cast<std::uint32_t>(at),static_cast<std::uint32_t>(length),content});
    }
    if (result.empty()) throw std::invalid_argument("Packed layout has no regions");
    return result;
}
}
