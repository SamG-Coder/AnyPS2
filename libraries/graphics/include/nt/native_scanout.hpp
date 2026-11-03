#pragma once
#include <nt/image_upload.hpp>
#include <optional>

namespace nt::graphics {
struct Scanout {
    unsigned width=0,height=0;
    std::vector<std::uint32_t> pixels;
};
struct DisplaySurface { std::uint32_t base=0,pitch=0,x=0,y=0,width=0,height=0; };
struct DisplayPlan {
    std::array<DisplaySurface,2> surfaces{};
    std::uint32_t width=0,height=0,control=0,background=0,half_height=0,bgra=0;
};
inline std::optional<DisplayPlan> plan_display(std::uint64_t control,
    const std::array<std::uint64_t,2>& buffers,const std::array<std::uint64_t,2>& displays,
    bool half_height=false,std::uint32_t background=0) {
    const auto enabled=control&3;
    if (!enabled) return {};
    if (enabled==3 && (!(control&32) || std::uint32_t(displays[0])!=std::uint32_t(displays[1])))
        throw std::invalid_argument("Unsupported native display composition");
    DisplayPlan plan{}; plan.control=std::uint32_t(control); plan.background=background; plan.half_height=half_height;
    for (unsigned i=0;i<2;++i) if (enabled&(1U<<i)) {
        const auto b=buffers[i],d=displays[i];
        auto& surface=plan.surfaces[i];
        surface={unsigned(b&511)*32,unsigned((b>>9)&63),unsigned((b>>32)&2047),unsigned((b>>43)&2047),
            unsigned((((d>>32)&4095)+1)/(((d>>23)&15)+1)),
            unsigned((((d>>44)&2047)+1)/(((d>>27)&3)+1))};
        if (((b>>15)&31)>1 || !surface.pitch) throw std::invalid_argument("Unsupported native display surface");
        if (!surface.width || !surface.height || surface.width>2048 || surface.height>2048)
            throw std::invalid_argument("Invalid native display extent");
        plan.width=std::max(plan.width,surface.width); plan.height=std::max(plan.height,surface.height);
    }
    return plan;
}
// Convert one enabled display circuit's 24/32-bit target to a native image.
// Interlaced frame mode stores half as many source rows. Present each source
// row twice on a progressive PC display; temporal field blending is separate.
// Aligned dual-circuit output supports constant-alpha composition, including
// adjacent-row flicker filters. Dynamic-alpha and displaced outputs stop.
inline std::optional<Scanout> scanout(std::span<const std::uint32_t> memory,std::uint64_t control,
    const std::array<std::uint64_t,2>& buffers,const std::array<std::uint64_t,2>& displays,bool half_height=false,
    std::uint32_t background=0) {
    const auto enabled=control&3;
    if (!enabled) return {};
    if (memory.size()!=1024*1024) throw std::invalid_argument("Invalid native display memory size");
    if (enabled==3) {
        if (!(control&32) || (std::uint32_t(displays[0])!=std::uint32_t(displays[1])))
            throw std::invalid_argument("Unsupported native display composition");
        const auto first=scanout(memory,1,buffers,displays,half_height);
        const auto second=scanout(memory,2,buffers,displays,half_height);
        const unsigned alpha=(control>>8)&255;
        Scanout frame{std::max(first->width,second->width),std::max(first->height,second->height),{}};
        frame.pixels.resize(std::size_t(frame.width)*frame.height);
        for (unsigned y=0;y<frame.height;++y) for (unsigned x=0;x<frame.width;++x) {
            auto color=background|0xff000000U;
            if (!(control&128) && x<second->width && y<second->height) color=second->pixels[y*second->width+x];
            if (x<first->width && y<first->height) {
                const auto source=first->pixels[y*first->width+x];
                std::uint32_t blended=0xff000000U;
                for (unsigned shift=0;shift<24;shift+=8)
                    blended|=((((source>>shift)&255)*alpha+((color>>shift)&255)*(255-alpha)+127)/255)<<shift;
                color=blended;
            }
            frame.pixels[y*frame.width+x]=color;
        }
        return frame;
    }
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
