#pragma once
#include <nt/image_upload.hpp>

namespace nt::graphics {
// A host readback preserves the stored pixel bits, rather than display RGBA.
// Reuse upload extent/format validation with source fields in destination slots.
struct ImageDownload {
    ImageUpload source;
    static ImageDownload begin(std::uint64_t buffer,std::uint64_t position,std::uint64_t extent) {
        return {ImageUpload::begin(buffer<<32,position<<32,extent,0)};
    }
    // The lowest and highest memory word the rectangle reads.
    std::pair<unsigned,unsigned> word_range() const {
        const auto& s=source; unsigned low=~0U,high=0;
        for (unsigned y=0;y<s.height;++y) for (unsigned x=0;x<s.width;++x) {
            unsigned word;
            if (s.bits==16) word=rgba_halfword(s.base,s.pitch,s.x+x,s.y+y,s.alternate_16)/2;
            else if (s.bits==8) word=indexed_byte(s.base,s.pitch,s.x+x,s.y+y)/4;
            else if (s.bits==4) word=indexed_nibble(s.base,s.pitch,s.x+x,s.y+y)/8;
            else word=rgba_word(s.base,s.pitch,s.x+x,s.y+y);
            low=std::min(low,word); high=std::max(high,word);
        }
        return {low,high};
    }
    std::vector<std::uint8_t> pack(std::span<const std::uint32_t> words) const {
        if (words.size()!=1024*1024) throw std::invalid_argument("Invalid native readback storage");
        const auto& s=source;
        const auto bytes=(std::size_t(s.width)*s.height*s.bits+7)/8;
        // The final transport word is padded; bytes outside the rectangle do
        // not address additional pixels.
        std::vector<std::uint8_t> result((bytes+15)&~std::size_t(15));
        std::size_t bit=0;
        for (unsigned y=0;y<s.height;++y) for (unsigned x=0;x<s.width;++x) {
            std::uint32_t value;
            if (s.bits==16) {
                const auto a=rgba_halfword(s.base,s.pitch,s.x+x,s.y+y,s.alternate_16);
                value=(words[a/2]>>((a%2)*16))&65535;
            } else if (s.bits==8) {
                const auto a=indexed_byte(s.base,s.pitch,s.x+x,s.y+y);
                value=(words[a/4]>>((a%4)*8))&255;
            } else if (s.bits==4) {
                const auto a=indexed_nibble(s.base,s.pitch,s.x+x,s.y+y);
                value=(words[a/8]>>((a%8)*4))&15;
            } else value=words[rgba_word(s.base,s.pitch,s.x+x,s.y+y)];
            if (s.bits==4) { result[bit/8]|=std::uint8_t(value<<(bit%8)); bit+=4; }
            else for (unsigned b=0;b<s.bits;b+=8,bit+=8) result[bit/8]=std::uint8_t(value>>b);
        }
        return result;
    }
};
}
