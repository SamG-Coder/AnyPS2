#pragma once
#include <nt/binary.hpp>
namespace nt::package {
struct Segment { std::uint32_t address; Bytes data; };
struct Image { std::uint32_t entry; std::vector<Segment> segments; };
// Unencrypted, length-prefixed raw DEFLATE with bounded segmented-image payload.
// Opaque metadata is skipped, not authenticated. No filename/title selection.
Image unpack(ByteView input);
// Equal overlapping bytes are coalesced. Conflicting overlaps reject input.
// Container has no section permissions: only the entry-containing segment is
// initially executable. Other code segments require explicit conversion metadata;
// discovery must report unresolved transfers rather than guessing data is code.
Bytes elf_image(const Image& image);
}
