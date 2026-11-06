#pragma once
#include <nt/graphics_packet.hpp>
#include <nt/pixel_address.hpp>
#include <algorithm>
#include <string>

namespace nt::graphics {
struct PixelWrite { unsigned address; std::uint32_t value,mask; };
// A run of `count` pixels of a host-to-local transfer rectangle, in transfer
// order starting at pixel `first` (row-major within `width`). `words` holds
// the packed pixel stream (32, 16, 8 or 4 bits per pixel).
struct ImageRect {
    unsigned base=0,pitch=0,x=0,y=0,width=0,first=0,bits=32,count=0;
    bool alternate_16=false,depth_layout=false;
    std::vector<std::uint32_t> words;
    std::uint32_t value(std::size_t index) const {
        if (bits==32) return words[index];
        const auto bit=index*bits;
        return (words[bit/32]>>(bit%32))&((1U<<bits)-1);
    }
    // The memory word, bit shift and unshifted mask a pixel occupies.
    void place(std::size_t index,unsigned& word,unsigned& shift,std::uint32_t& mask) const {
        const auto pixel=first+unsigned(index),px=x+pixel%width,py=y+pixel/width;
        if (bits==32) { word=stored_word(base,pitch,px,py,depth_layout); shift=0; mask=0xffffffffU; }
        else if (bits==16) { const auto half=rgba_halfword(base,pitch,px,py,alternate_16); word=half/2; shift=(half%2)*16; mask=65535U; }
        else if (bits==8) { const auto byte=indexed_byte(base,pitch,px,py); word=byte/4; shift=(byte%4)*8; mask=255U; }
        else { const auto nibble=indexed_nibble(base,pitch,px,py); word=nibble/8; shift=(nibble%8)*4; mask=15U; }
    }
    unsigned address(std::size_t index) const { unsigned word,shift; std::uint32_t mask; place(index,word,shift,mask); return word; }
    std::vector<PixelWrite> writes() const {
        std::vector<PixelWrite> result(count);
        for (std::size_t i=0;i<count;++i) {
            unsigned word,shift; std::uint32_t mask; place(i,word,shift,mask);
            result[i]={word,(value(i)&mask)<<shift,mask<<shift};
        }
        return result;
    }
};
struct ImageUpload {
    unsigned base=0,pitch=0,x=0,y=0,width=0,height=0,bits=0,pixel=0,pending_bits=0;
    std::uint64_t pending=0;
    bool alternate_16=false,depth_layout=false;
    static ImageUpload begin(std::uint64_t buffer,std::uint64_t position,std::uint64_t extent,unsigned direction) {
        if (direction==3) return {};
        if (direction) throw std::invalid_argument("Native image upload requires host-to-local direction");
        const unsigned format=(buffer>>56)&63;
        const unsigned traversal=(position>>59)&3;
        const bool depth_layout=format==0x30 || format==0x31;
        if ((format>2 && format!=10 && format!=19 && format!=20 && !depth_layout) || traversal)
            throw std::invalid_argument("Unsupported native image upload: format="+std::to_string(format)+
                " traversal="+std::to_string(traversal));
        ImageUpload upload;
        upload.base=(buffer>>32)&16383; upload.pitch=(buffer>>48)&63;
        upload.x=(position>>32)&2047; upload.y=(position>>48)&2047;
        upload.width=extent&4095; upload.height=(extent>>32)&4095;
        if (!upload.pitch || !upload.width || !upload.height) throw std::invalid_argument("Empty native image upload surface");
        upload.bits=format==20 ? 4 : format==19 ? 8 : (format==2 || format==10) ? 16 : (format==1 || format==0x31) ? 24 : 32;
        upload.alternate_16=format==10; upload.depth_layout=depth_layout;
        return upload;
    }
    // Data arriving on a pixel boundary, other than 24-bit, is kept as a
    // rectangle run; quadword packets keep every later packet aligned too.
    bool whole_words() const { return bits && bits!=24 && pending_bits==0; }
    void append(std::span<const Quadword> values,ImageRect& rect) {
        rect.base=base; rect.pitch=pitch; rect.x=x; rect.y=y; rect.width=width; rect.first=pixel;
        rect.bits=bits; rect.alternate_16=alternate_16; rect.depth_layout=depth_layout;
        if (pixel>=width*height) return;
        rect.count=unsigned(std::min<std::size_t>(std::size_t(width)*height-pixel,values.size()*128/bits));
        const auto used=(std::size_t(rect.count)*bits+31)/32;
        rect.words.resize(used);
        for (std::size_t i=0;i<used;++i) rect.words[i]=values[i/4][i%4];
        pixel+=rect.count;
    }
    void append(const Quadword& value,std::vector<PixelWrite>& writes) {
        append(std::span(&value,1),writes);
    }
    void append(std::span<const Quadword> values,std::vector<PixelWrite>& writes) {
        if (values.empty()) return;
        if (bits!=4 && bits!=8 && bits!=16 && bits!=24 && bits!=32) throw std::invalid_argument("Image payload without active native upload");
        // IMAGE packets may contain more data than the declared rectangle.
        // Once it is complete, consume transport padding without writing pixels.
        if (pixel>=width*height) return;
        unsigned column=pixel%width,row=pixel/width;
        const auto advance=[&] {
            ++pixel;
            if (++column==width) { column=0; ++row; }
        };
        for (const auto& value:values) {
            if (pixel>=width*height) break;
            if (bits==32 && pending_bits==0) {
                for (unsigned word=0;word<4 && pixel<width*height;++word) {
                    writes.push_back({stored_word(base,pitch,x+column,y+row,depth_layout),value[word],0xffffffffU});
                    advance();
                }
                continue;
            }
            for (unsigned word=0;word<4 && pixel<width*height;++word) {
                // Consume only whole bytes required by the remaining pixels.
                // This retains the exact partial-byte state and ignores padding.
                const unsigned needed=(width*height-pixel)*bits-pending_bits;
                const unsigned incoming=std::min(32U,(needed+7)&~7U);
                const auto mask=incoming==32 ? 0xffffffffU : (1U<<incoming)-1;
                pending|=std::uint64_t(value[word]&mask)<<pending_bits;
                pending_bits+=incoming;
                while (pending_bits>=bits && pixel<width*height) {
                    if (bits==4) {
                        const auto nibble=indexed_nibble(base,pitch,x+column,y+row),shift=(nibble%8)*4;
                        writes.push_back({nibble/8,(std::uint32_t(pending)&15U)<<shift,15U<<shift});
                    } else if (bits==8) {
                        const auto byte=indexed_byte(base,pitch,x+column,y+row),shift=(byte%4)*8;
                        writes.push_back({byte/4,(std::uint32_t(pending)&255U)<<shift,255U<<shift});
                    } else if (bits==16) {
                        const auto half=rgba_halfword(base,pitch,x+column,y+row,alternate_16),shift=(half%2)*16;
                        writes.push_back({half/2,(std::uint32_t(pending)&65535U)<<shift,65535U<<shift});
                    } else {
                        const auto pixel_mask=bits==32 ? 0xffffffffU : 0xffffffU;
                        writes.push_back({stored_word(base,pitch,x+column,y+row,depth_layout),std::uint32_t(pending)&pixel_mask,pixel_mask});
                    }
                    pending>>=bits; pending_bits-=bits; advance();
                }
            }
        }
    }
};
}
