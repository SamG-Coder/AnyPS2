#pragma once
#include <nt/image_upload.hpp>
#include <optional>

namespace nt::graphics {
struct Scanout {
    unsigned width=0,height=0;
    std::vector<std::uint32_t> pixels;
};
struct DisplaySurface { std::uint32_t base=0,pitch=0,x=0,y=0,width=0,height=0,offset_x=0,offset_y=0; };
enum class DisplayField : std::uint32_t { progressive,even,odd };
struct DisplayPlan {
    std::array<DisplaySurface,2> surfaces{};
    std::uint32_t width=0,height=0,control=0,background=0,half_height=0,bgra=0;
    DisplayField field=DisplayField::progressive;
    std::uint32_t history_valid=0;
    // Presentation only: weight in 1/256 of each vertically adjacent display
    // row. It reproduces a CRT's averaging of sub-pixel drawing-offset jitter
    // that rendering suppresses (see NativeDrawing::offset_jitter).
    std::uint32_t smoothing=0;
};
// Neighbour weight for a jitter of +/- jitter_sixteenths of a pixel: the mean
// of the image at -a, 0 and +a lines (linear between rows) gives each
// neighbouring row a/3. Half-height output doubles every source line.
inline std::uint32_t jitter_smoothing(unsigned jitter_sixteenths,bool half_height) {
    const unsigned lines_256=jitter_sixteenths*16U*(half_height ? 2U : 1U);
    return std::min(lines_256,256U)/3U;
}
inline std::optional<DisplayPlan> plan_display(std::uint64_t control,
    const std::array<std::uint64_t,2>& buffers,const std::array<std::uint64_t,2>& displays,
    bool half_height=false,std::uint32_t background=0,DisplayField field=DisplayField::progressive) {
    const auto enabled=control&3;
    if (!enabled) return {};
    if (enabled==3 && !(control&32))
        throw std::invalid_argument("Unsupported native display composition");
    DisplayPlan plan{}; plan.control=std::uint32_t(control); plan.background=background; plan.half_height=half_height;
    plan.field=half_height ? field : DisplayField::progressive;
    const unsigned first=enabled==2 ? 1 : 0;
    const unsigned scale_x=((displays[first]>>23)&15)+1,scale_y=((displays[first]>>27)&3)+1;
    unsigned origin_x=displays[first]&4095,origin_y=(displays[first]>>12)&2047;
    if (enabled==3) {
        if ((((displays[1]>>23)&15)+1)!=scale_x || (((displays[1]>>27)&3)+1)!=scale_y)
            throw std::invalid_argument("Mixed display circuit magnification is unsupported");
        origin_x=std::min(origin_x,unsigned(displays[1]&4095));
        origin_y=std::min(origin_y,unsigned((displays[1]>>12)&2047));
    }
    for (unsigned i=0;i<2;++i) if (enabled&(1U<<i)) {
        const auto b=buffers[i],d=displays[i];
        auto& surface=plan.surfaces[i];
        surface={unsigned(b&511)*32,unsigned((b>>9)&63),unsigned((b>>32)&2047),unsigned((b>>43)&2047),
            unsigned((((d>>32)&4095)+1)/(((d>>23)&15)+1)),
            unsigned((((d>>44)&2047)+1)/(((d>>27)&3)+1))};
        if (((b>>15)&31)>1 || !surface.pitch) throw std::invalid_argument("Unsupported native display surface");
        if (!surface.width || !surface.height || surface.width>2048 || surface.height>2048)
            throw std::invalid_argument("Invalid native display extent");
        const unsigned dx=unsigned(d&4095)-origin_x,dy=unsigned((d>>12)&2047)-origin_y;
        if (dx%scale_x || dy%scale_y)
            throw std::invalid_argument("Fractional display circuit displacement is unsupported");
        surface.offset_x=dx/scale_x; surface.offset_y=dy/scale_y;
        plan.width=std::max(plan.width,surface.offset_x+surface.width);
        plan.height=std::max(plan.height,surface.offset_y+surface.height);
    }
    if (plan.width>2048 || plan.height>2048) throw std::invalid_argument("Native display composition exceeds output bounds");
    return plan;
}
// Offsets position circuits in a shared output rectangle. Source coordinates
// remain local to each circuit, including interlaced frame-mode row doubling.
inline Scanout scanout(std::span<const std::uint32_t> memory,const DisplayPlan& plan) {
    if (memory.size()!=1024*1024) throw std::invalid_argument("Invalid native display memory size");
    const auto enabled=plan.control&3;
    Scanout frame{plan.width,plan.height,std::vector<std::uint32_t>(std::size_t(plan.width)*plan.height)};
    const auto contains=[&](unsigned circuit,unsigned x,unsigned y) {
        const auto& s=plan.surfaces[circuit];
        return x>=s.offset_x && y>=s.offset_y && x-s.offset_x<s.width && y-s.offset_y<s.height;
    };
    const auto sample=[&](unsigned circuit,unsigned x,unsigned y) {
        const auto& s=plan.surfaces[circuit]; x-=s.offset_x; y-=s.offset_y;
        return memory[rgba_word(s.base,s.pitch,s.x+x,s.y+(plan.half_height ? y/2 : y))]|0xff000000U;
    };
    const auto compose=[&](unsigned x,unsigned y) {
        auto color=plan.background|0xff000000U;
        if (enabled==3 || (enabled==1 && (plan.control&32))) {
            if ((enabled&2) && !(plan.control&128) && contains(1,x,y)) color=sample(1,x,y);
            if (contains(0,x,y)) {
                const auto source=sample(0,x,y),alpha=(plan.control>>8)&255;
                std::uint32_t blended=0xff000000U;
                for (unsigned shift=0;shift<24;shift+=8)
                    blended|=((((source>>shift)&255)*alpha+((color>>shift)&255)*(255-alpha)+127)/255)<<shift;
                color=blended;
            }
        } else {
            const unsigned circuit=enabled==2 ? 1:0;
            if (contains(circuit,x,y)) color=sample(circuit,x,y);
        }
        return color;
    };
    // Reconstruct missing lines spatially at the current field's position.
    // Do not weave different moments together or shift both fields onto row 0.
    for (unsigned y=0;y<frame.height;++y) for (unsigned x=0;x<frame.width;++x) {
        std::uint32_t color;
        if (plan.field!=DisplayField::progressive) {
            const unsigned phase=plan.field==DisplayField::odd ? 1:0;
            const unsigned row=(y>phase ? y-phase : 0)/2*2;
            color=compose(x,row);
            if (y>=phase && ((y-phase)&1)) {
                const auto next=compose(x,std::min(row+2,frame.height-1));
                std::uint32_t blended=0xff000000U;
                for (unsigned shift=0;shift<24;shift+=8)
                    blended|=(((((color>>shift)&255)+((next>>shift)&255)+1)/2)<<shift);
                color=blended;
            }
        } else color=compose(x,y);
        frame.pixels[y*frame.width+x]=color;
    }
    return frame;
}
inline std::optional<Scanout> scanout(std::span<const std::uint32_t> memory,std::uint64_t control,
    const std::array<std::uint64_t,2>& buffers,const std::array<std::uint64_t,2>& displays,bool half_height=false,
    std::uint32_t background=0,DisplayField field=DisplayField::progressive) {
    const auto plan=plan_display(control,buffers,displays,half_height,background,field);
    return plan ? std::optional(scanout(memory,*plan)) : std::nullopt;
}
}
