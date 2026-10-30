#include <nt/gs.hpp>
#include <nt/palette_plan.hpp>

namespace nt::hardware {
void Gs::texture_register(unsigned reg, std::uint64_t value) {
    auto bases=palette_base_;
    try {
        if (const auto load=graphics::plan_palette(value,drawing_[0x1c],bases))
            graphics::load_palette(palette_,*local_,*load);
    } catch (const std::invalid_argument& error) { throw runtime::MemoryFault(error.what()); }
    palette_base_=bases;
    drawing_[reg]=value;
}
}
