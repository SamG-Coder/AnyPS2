#pragma once
#include <nt/graphics_packet.hpp>
#include <nt/pixel_address.hpp>
#include <algorithm>

namespace nt::graphics {
struct PixelWrite { unsigned address; std::uint32_t value,mask; };
struct ImageUpload {
    unsigned base=0,pitch=0,x=0,y=0,width=0,height=0,bits=0,pixel=0,pending_bits=0;
    std::uint64_t pending=0;
    static ImageUpload begin(std::uint64_t buffer,std::uint64_t position,std::uint64_t extent,unsigned direction) {
        if (direction==3) return {};
        if (direction) throw std::invalid_argument("Native image upload requires host-to-local direction");
        const unsigned format=(buffer>>56)&63;
        if ((format>1 && format!=19 && format!=20) || ((position>>59)&3)) throw std::invalid_argument("Unsupported native image format or traversal");
        ImageUpload upload;
        upload.base=(buffer>>32)&16383; upload.pitch=(buffer>>48)&63;
        upload.x=(position>>32)&2047; upload.y=(position>>48)&2047;
        upload.width=extent&4095; upload.height=(extent>>32)&4095;
        if (!upload.pitch || !upload.width || !upload.height) throw std::invalid_argument("Empty native image upload surface");
        upload.bits=format==20 ? 4 : format==19 ? 8 : format ? 24 : 32;
        return upload;
    }
    void append(const Quadword& value,std::vector<PixelWrite>& writes) {
        append(std::span(&value,1),writes);
    }
    void append(std::span<const Quadword> values,std::vector<PixelWrite>& writes) {
        if (values.empty()) return;
        if (bits!=4 && bits!=8 && bits!=24 && bits!=32) throw std::invalid_argument("Image payload without active native upload");
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
                    writes.push_back({rgba_word(base,pitch,x+column,y+row),value[word],0xffffffffU});
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
                    } else {
                        const auto pixel_mask=bits==32 ? 0xffffffffU : 0xffffffU;
                        writes.push_back({rgba_word(base,pitch,x+column,y+row),std::uint32_t(pending)&pixel_mask,pixel_mask});
                    }
                    pending>>=bits; pending_bits-=bits; advance();
                }
            }
        }
    }
};
}
