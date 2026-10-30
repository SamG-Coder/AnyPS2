#include <nt/vu_program.hpp>
#include <stdexcept>

namespace nt::vu {
namespace {
Operation fields(std::uint32_t word) {
    return {word,word>>25,word&63U,(word>>6)&31U,(word>>11)&31U,
        (word>>16)&31U,(word>>6)&31U,(word>>21)&15U};
}
}
std::vector<Pair> frame(ByteView bytes,unsigned unit,unsigned destination) {
    if (unit>1 || bytes.empty() || bytes.size()%8 || bytes.size()>16384)
        throw std::invalid_argument("Invalid VU program extent or unit");
    const auto capacity=unit ? 2048U : 512U;
    if (destination>=capacity || bytes.size()/8>capacity-destination)
        throw std::invalid_argument("VU program crosses instruction storage boundary");
    std::vector<Pair> pairs;
    pairs.reserve(bytes.size()/8);
    for (std::size_t offset=0;offset<bytes.size();offset+=8) {
        const auto lower=le32(bytes,offset),upper=le32(bytes,offset+4);
        Pair pair{destination+static_cast<unsigned>(offset/8),fields(upper),{},{},
            (upper&0x40000000U)!=0,(upper&0x20000000U)!=0,
            (upper&0x10000000U)!=0,(upper&0x08000000U)!=0};
        if (upper&0x80000000U) pair.immediate=lower;
        else pair.lower=fields(lower);
        pairs.push_back(pair);
    }
    return pairs;
}
}
