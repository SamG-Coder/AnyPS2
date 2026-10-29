#include <nt/graphics_dma.hpp>

namespace nt::hardware {
namespace {
std::uint32_t memory_address(std::uint32_t address) {
    // DMAC's high bit selects scratchpad, rather than a cached EE alias.
    return (address & 0x80000000U) ? 0x70000000U | (address & 0x3fff) : address & 0x1ffffff0U;
}
std::array<std::uint32_t,4> words(runtime::Register quad) {
    return {static_cast<std::uint32_t>(quad.lo),static_cast<std::uint32_t>(quad.lo>>32),
            static_cast<std::uint32_t>(quad.hi),static_cast<std::uint32_t>(quad.hi>>32)};
}
}
std::uint32_t GraphicsDma::step(runtime::RamBus& ram, Vif& vif, Gif& gif, Gs& gs, bool enabled) {
    if (!enabled) return 0;
    std::uint32_t completed = 0;
    for (unsigned unit=0;unit<3;++unit) {
        auto& c = channels_[unit];
        if (!(c[0] & 0x100) || (unit == 2 ? gif.stopped() : vif.stopped(unit))) continue;
        if (c[2]) {
            const auto payload = ram.load_quad(memory_address(c[1]));
            if (unit == 2) {
                if (!gif.accepts(3)) continue;
                gif.consume(payload,gs);
            } else if (!vif.consume(unit,words(payload),&gif,&gs)) continue;
            c[1] += 16; --c[2];
        } else if (!end_[unit]) {
            auto next = c;
            const auto tag = words(ram.load_quad(memory_address(c[3])));
            const auto id = (tag[0] >> 28) & 7;
            const auto count = tag[0] & 65535;
            const auto after = c[3]+16;
            next[2] = count;
            next[0] = (c[0] & 65535) | (tag[0] & 0xffff0000);
            switch (id) {
            case 0: case 3: next[1] = tag[1]; next[3] = after; break; // REFE / REF
            case 1: next[1] = after; next[3] = after+count*16; break; // CNT
            case 2: next[1] = after; next[3] = tag[1]; break; // NEXT
            case 7: next[1] = after; break; // END preserves TADR.
            default: throw runtime::MemoryFault("Graphics DMA: source tag type is not implemented");
            }
            if ((c[0] & 0x40) && !vif.consume(unit,std::span(tag).subspan(2),&gif,&gs)) continue;
            end_[unit] = id == 0 || id == 7 || ((c[0] & 0x80) && (tag[0] & 0x80000000U));
            c = next;
        }
        if (!c[2] && end_[unit]) { c[0] &= ~0x100U; completed |= 1U << unit; }
    }
    return completed;
}
}
