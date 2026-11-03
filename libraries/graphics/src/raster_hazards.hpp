#pragma once
#include <array>
#include <cstdint>
#include <algorithm>

namespace nt::graphics {
// Conservative screen-space dependencies for ordered attachment reads/writes.
// Tiles may add barriers, but cannot omit an overlapping pixel dependency.
class RasterHazards {
public:
    void clear() { tiles_.fill(0); }
    bool overlaps(unsigned left,unsigned top,unsigned right,unsigned bottom) const {
        bool found=false;
        visit(left,top,right,bottom,[&](unsigned index,std::uint64_t mask) { found|=(tiles_[index]&mask)!=0; });
        return found;
    }
    void add(unsigned left,unsigned top,unsigned right,unsigned bottom) {
        visit(left,top,right,bottom,[&](unsigned index,std::uint64_t mask) { tiles_[index]|=mask; });
    }
private:
    std::array<std::uint64_t,512> tiles_{}; // 4096 x 2048 pixels, 16 x 16 tiles.
    template<class F> static void visit(unsigned left,unsigned top,unsigned right,unsigned bottom,F&& f) {
        if (left>=right || top>=bottom) return;
        const unsigned x0=left/16,x1=(right-1)/16;
        for (unsigned y=top/16;y<=(bottom-1)/16;++y) {
            for (unsigned word=x0/64;word<=x1/64;++word) {
                const unsigned low=std::max(x0,word*64)-word*64;
                const unsigned high=std::min(x1,word*64+63)-word*64;
                f(y*4+word,(~std::uint64_t(0)<<low)&(~std::uint64_t(0)>>(63-high)));
            }
        }
    }
};
}
