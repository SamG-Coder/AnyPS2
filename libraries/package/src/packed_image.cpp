#include <nt/packed_image.hpp>
#include <zlib.h>
#include <algorithm>
#include <stdexcept>

namespace nt::package {
namespace {
constexpr std::size_t limit=32U*1024*1024;
void put16(Bytes& data,std::size_t at,std::uint16_t value) {
    data.at(at)=static_cast<std::uint8_t>(value); data.at(at+1)=static_cast<std::uint8_t>(value>>8);
}
void put32(Bytes& data,std::size_t at,std::uint32_t value) {
    for (unsigned n=0;n<4;++n) data.at(at+n)=static_cast<std::uint8_t>(value>>(8*n));
}
}
Image unpack(ByteView input) {
    require_range(input,0,6);
    const auto expected=le32(input,2);
    if (!expected || expected>limit || input.size()>limit) throw std::runtime_error("Packed image size exceeds conversion bound");
    Bytes decoded(expected+1); // Detect excess output without accepting truncation.
    z_stream stream{};
    stream.next_in=const_cast<Bytef*>(input.data()+6); stream.avail_in=static_cast<uInt>(input.size()-6);
    stream.next_out=decoded.data(); stream.avail_out=static_cast<uInt>(decoded.size());
    if (inflateInit2(&stream,-MAX_WBITS)!=Z_OK) throw std::runtime_error("Cannot initialize DEFLATE decoder");
    const auto status=inflate(&stream,Z_FINISH);
    const bool valid=status==Z_STREAM_END && stream.total_out==expected && !stream.avail_in;
    inflateEnd(&stream);
    if (!valid) throw std::runtime_error("Packed image has malformed DEFLATE, size mismatch or trailing bytes");
    decoded.resize(expected);
    std::size_t cursor=0;
    for (unsigned n=0;n<2;++n) {
        const auto size=le16(decoded,cursor); cursor+=2;
        require_range(decoded,cursor,size); cursor+=size;
    }
    const auto count=le32(decoded,cursor),entry=le32(decoded,cursor+4); cursor+=8;
    if (!count || count>64 || !entry || entry%4 || entry>=limit) throw std::runtime_error("Invalid segmented-image header");
    Image image{entry,{}};
    for (unsigned n=0;n<count;++n) {
        const auto address=le32(decoded,cursor),size=le32(decoded,cursor+4); cursor+=8;
        if (!size || std::uint64_t(address)+size>limit) throw std::runtime_error("Segment outside native RAM");
        require_range(decoded,cursor,size);
        image.segments.push_back({address,Bytes(decoded.begin()+cursor,decoded.begin()+cursor+size)});
        cursor+=size;
    }
    if (cursor!=decoded.size()) throw std::runtime_error("Trailing segmented-image data");
    return image;
}
Bytes elf_image(const Image& image) {
    if (image.segments.empty() || image.segments.size()>64 || !image.entry || image.entry%4)
        throw std::runtime_error("Invalid segmented image");
    auto source=image.segments;
    std::sort(source.begin(),source.end(),[](const auto& a,const auto& b) { return a.address<b.address; });
    std::vector<Segment> segments;
    bool entry_present=false;
    for (const auto& segment:source) {
        const auto end=std::uint64_t(segment.address)+segment.data.size();
        if (segment.data.empty() || end>limit || segment.address%4) throw std::runtime_error("Invalid native segment extent");
        entry_present|=image.entry>=segment.address && std::uint64_t(image.entry)+4<=end;
        if (!segments.empty()) {
            auto& previous=segments.back();
            const auto previous_end=std::uint64_t(previous.address)+previous.data.size();
            if (segment.address<=previous_end) {
                const auto offset=segment.address-previous.address;
                const auto overlap=std::min(segment.data.size(),previous.data.size()-offset);
                if (!std::equal(segment.data.begin(),segment.data.begin()+overlap,previous.data.begin()+offset))
                    throw std::runtime_error("Conflicting segment overlap");
                previous.data.insert(previous.data.end(),segment.data.begin()+overlap,segment.data.end());
                continue;
            }
        }
        segments.push_back(segment);
    }
    if (!entry_present) throw std::runtime_error("Entry is not backed by executable input");
    Bytes result(52+32*segments.size());
    const std::uint8_t identity[]{0x7f,'E','L','F',1,1,1};
    std::copy(std::begin(identity),std::end(identity),result.begin());
    put16(result,16,2); put16(result,18,8); put32(result,20,1); put32(result,24,image.entry);
    put32(result,28,52); put16(result,40,52); put16(result,42,32); put16(result,44,static_cast<std::uint16_t>(segments.size()));
    for (std::size_t n=0;n<segments.size();++n) {
        const auto& segment=segments[n]; const auto at=52+32*n;
        put32(result,at,1); put32(result,at+4,static_cast<std::uint32_t>(result.size()));
        put32(result,at+8,segment.address); put32(result,at+12,segment.address);
        put32(result,at+16,static_cast<std::uint32_t>(segment.data.size()));
        put32(result,at+20,static_cast<std::uint32_t>(segment.data.size()));
        const bool entry_segment=image.entry>=segment.address && std::uint64_t(image.entry)<std::uint64_t(segment.address)+segment.data.size();
        put32(result,at+24,entry_segment ? 7U : 6U); put32(result,at+28,4);
        result.insert(result.end(),segment.data.begin(),segment.data.end());
    }
    return result;
}
}
