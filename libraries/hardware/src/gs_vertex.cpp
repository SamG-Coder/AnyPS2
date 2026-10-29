#include <nt/gs.hpp>

namespace nt::hardware {
void Gs::submit_vertex(unsigned reg, std::uint64_t value) {
    const unsigned primitive = drawing_[0] & 7;
    if (primitive == 7)
        throw runtime::MemoryFault("GS: reserved primitive type");
    constexpr std::array<unsigned,7> counts{1,2,2,3,3,3,2};
    const auto required = counts[primitive];
    const bool fogged = reg == 4 || reg == 12;
    Vertex vertex;
    vertex.x = static_cast<std::uint16_t>(value);
    vertex.y = static_cast<std::uint16_t>(value >> 16);
    vertex.z = static_cast<std::uint32_t>(value >> 32) & (fogged ? 0xffffffU : 0xffffffffU);
    vertex.fog = static_cast<std::uint8_t>((fogged ? value : drawing_[10]) >> 56);
    vertex.rgbaq = drawing_[1]; vertex.st = drawing_[2]; vertex.uv = drawing_[3];
    // XYZ3/XYZF3 still advance topology but suppress this drawing kick.
    // Validate before changing registers or the pending primitive.
    if (vertex_count_+1 == required && reg < 12) {
        if (primitive == 6) draw_sprite(vertices_[0],vertex);
        else throw runtime::MemoryFault("GS: primitive rasterization is not implemented (type " +
                                        std::to_string(primitive) + ")");
    }
    vertices_[vertex_count_++] = vertex;
    drawing_[fogged ? 4 : 5] = value;
    if (fogged) drawing_[10] = value & 0xff00000000000000ULL;
    if (vertex_count_ < required) return;
    if (primitive == 2 || primitive == 4) {
        for (unsigned i=1;i<required;++i) vertices_[i-1] = vertices_[i];
        vertex_count_ = required-1;
    } else if (primitive == 5) {
        vertices_[1] = vertices_[2]; // Preserve the fan anchor.
        vertex_count_ = 2;
    } else vertex_count_ = 0;
}
}
