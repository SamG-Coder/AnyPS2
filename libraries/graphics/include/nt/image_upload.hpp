#pragma once
#include <nt/graphics_packet.hpp>

namespace nt::graphics {
inline unsigned rgba_word(unsigned base,unsigned pitch,unsigned x,unsigned y) {
    x&=2047; y&=2047;
    const auto page=(y/32)*pitch+x/64;
    const auto block=((x>>3)&1)|(((y>>3)&1)<<1)|(((x>>4)&1)<<2)|(((y>>4)&1)<<3)|(((x>>5)&1)<<4);
    const auto word=(x&1)|((y&1)<<1)|((x&6)<<1)|((y&6)<<3);
    return (base*64+page*2048+block*64+word)&0xfffff;
}
struct PixelWrite { unsigned address; std::uint32_t value,mask; };
struct ImageUpload {
    unsigned base=0,pitch=0,x=0,y=0,width=0,height=0,bits=0,pixel=0,pending_bits=0;
    std::uint64_t pending=0;
    static ImageUpload begin(std::uint64_t buffer,std::uint64_t position,std::uint64_t extent,unsigned direction) {
        if (direction==3) return {};
        if (direction) throw std::invalid_argument("Native image upload requires host-to-local direction");
        const unsigned format=(buffer>>56)&63;
        if (format>1 || ((position>>59)&3)) throw std::invalid_argument("Unsupported native image format or traversal");
        ImageUpload upload;
        upload.base=(buffer>>32)&16383; upload.pitch=(buffer>>48)&63;
        upload.x=(position>>32)&2047; upload.y=(position>>48)&2047;
        upload.width=extent&4095; upload.height=(extent>>32)&4095;
        if (!upload.pitch || !upload.width || !upload.height) throw std::invalid_argument("Empty native image upload surface");
        upload.bits=format ? 24 : 32;
        return upload;
    }
    void append(const Quadword& value,std::vector<PixelWrite>& writes) {
        if (!bits || pixel>=width*height) throw std::invalid_argument("Image payload without active native upload");
        for (unsigned byte=0;byte<16 && pixel<width*height;++byte) {
            pending|=std::uint64_t((value[byte/4]>>((byte%4)*8))&255)<<pending_bits;
            pending_bits+=8;
            if (pending_bits<bits) continue;
            const auto mask=bits==32 ? 0xffffffffU : 0xffffffU;
            writes.push_back({rgba_word(base,pitch,x+pixel%width,y+pixel/width),std::uint32_t(pending)&mask,mask});
            pending>>=bits; pending_bits-=bits; ++pixel;
        }
    }
};
}
