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
            else word=stored_word(s.base,s.pitch,s.x+x,s.y+y,s.depth_layout);
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
            } else value=words[stored_word(s.base,s.pitch,s.x+x,s.y+y,s.depth_layout)];
            if (s.bits==4) { result[bit/8]|=std::uint8_t(value<<(bit%8)); bit+=4; }
            else for (unsigned b=0;b<s.bits;b+=8,bit+=8) result[bit/8]=std::uint8_t(value>>b);
        }
        return result;
    }
};
// A local-to-local transfer moves stored pixel bits between rectangles. Pixels
// run in the order TRXPOS selects, each seeing earlier writes, so overlapping
// copies match the GS. Differently sized formats would need GS format conversion.
struct ImageCopy {
    ImageDownload source;
    ImageUpload destination;
    unsigned direction=0;
    static ImageCopy begin(std::uint64_t buffer,std::uint64_t position,std::uint64_t extent) {
        const auto ordered=position&~(3ULL<<59);
        ImageCopy copy{ImageDownload::begin(buffer,ordered,extent),ImageUpload::begin(buffer,ordered,extent,0),unsigned(position>>59)&3};
        if (copy.source.source.bits!=copy.destination.bits)
            throw std::invalid_argument("Unsupported native image copy between pixel sizes: "+std::to_string(copy.source.source.bits)+
                " to "+std::to_string(copy.destination.bits));
        return copy;
    }
    // Applies the copy to words and returns the writes for the owner of memory.
    std::vector<PixelWrite> apply(std::span<std::uint32_t> words) const {
        if (words.size()!=1024*1024) throw std::invalid_argument("Invalid native copy storage");
        const auto& from=source.source; const auto& to=destination;
        std::vector<PixelWrite> writes;
        writes.reserve(std::size_t(to.width)*to.height);
        const auto place=[](const ImageUpload& image,unsigned x,unsigned y,unsigned& word,unsigned& shift,std::uint32_t& mask) {
            if (image.bits==16) { const auto a=rgba_halfword(image.base,image.pitch,image.x+x,image.y+y,image.alternate_16); word=a/2; shift=(a%2)*16; mask=65535U; }
            else if (image.bits==8) { const auto a=indexed_byte(image.base,image.pitch,image.x+x,image.y+y); word=a/4; shift=(a%4)*8; mask=255U; }
            else if (image.bits==4) { const auto a=indexed_nibble(image.base,image.pitch,image.x+x,image.y+y); word=a/8; shift=(a%8)*4; mask=15U; }
            else { word=stored_word(image.base,image.pitch,image.x+x,image.y+y,image.depth_layout); shift=0; mask=image.bits==24 ? 0xffffffU : 0xffffffffU; }
        };
        for (unsigned row=0;row<to.height;++row) for (unsigned column=0;column<to.width;++column) {
            const auto x=(direction&2) ? to.width-1-column : column;
            const auto y=(direction&1) ? to.height-1-row : row;
            unsigned word,shift; std::uint32_t mask;
            place(from,x,y,word,shift,mask);
            const auto value=(words[word]>>shift)&mask;
            place(to,x,y,word,shift,mask);
            words[word]=(words[word]&~(mask<<shift))|(value<<shift);
            writes.push_back({word,value<<shift,mask<<shift});
        }
        return writes;
    }
};
}
