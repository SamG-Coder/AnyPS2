#pragma once
#include <nt/image_upload.hpp>
#include <optional>

namespace nt::graphics {
struct Scanout {
    unsigned width=0,height=0;
    std::vector<std::uint32_t> pixels;
};
// Convert one enabled display circuit's 24/32-bit target to a native image.
// Interlaced frame mode stores half as many source rows. Present each source
// row twice on a progressive PC display; temporal field blending is separate.
// Dual-circuit blending requires separate support.
inline std::optional<Scanout> scanout(std::span<const std::uint32_t> memory,std::uint64_t control,
    const std::array<std::uint64_t,2>& buffers,const std::array<std::uint64_t,2>& displays,bool half_height=false) {
    const auto enabled=control&3;
    if (!enabled) return {};
    if (enabled==3 || memory.size()!=1024*1024) throw std::invalid_argument("Unsupported native display composition");
    const unsigned circuit=enabled==2 ? 1:0;
    const auto buffer=buffers[circuit],display=displays[circuit];
    const unsigned format=(buffer>>15)&31,pitch=(buffer>>9)&63;
    if (format>1 || !pitch) throw std::invalid_argument("Unsupported native display surface");
    const unsigned width=(((display>>32)&4095)+1)/(((display>>23)&15)+1);
    const unsigned height=(((display>>44)&2047)+1)/(((display>>27)&3)+1);
    if (!width || !height || width>2048 || height>2048) throw std::invalid_argument("Invalid native display extent");
    const unsigned x0=(buffer>>32)&2047,y0=(buffer>>43)&2047,base=(buffer&511)*32;
    Scanout frame{width,height,std::vector<std::uint32_t>(std::size_t(width)*height)};
    for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x)
        frame.pixels[y*width+x]=memory[rgba_word(base,pitch,x0+x,y0+(half_height ? y/2 : y))]|0xff000000U;
    return frame;
}
}
