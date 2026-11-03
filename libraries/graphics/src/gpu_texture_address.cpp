#include <nt/pixel_address.hpp>

extern "C" unsigned nt_gpu_texture_address(unsigned base,unsigned pitch,
                                            unsigned x,unsigned y,unsigned indexed) {
    return indexed ? nt::graphics::indexed_nibble(base,pitch,x,y) :
                     nt::graphics::rgba_word(base,pitch,x,y)*8;
}
