#pragma once
#include <nt/binary.hpp>
namespace nt::package {
struct Segment { std::uint32_t address; Bytes data; };
struct Image { std::uint32_t entry; std::vector<Segment> segments; };
// Unencrypted, length-prefixed raw DEFLATE with bounded segmented-image payload.
// Opaque metadata is skipped, not authenticated. No filename/title selection.
Image unpack(ByteView input);
// Equal overlapping bytes are coalesced. Conflicting overlaps reject input.
// Container has no section permissions. An AnyPS2 PT_NOTE import record allows
// static control-flow discovery in all file-backed segments. Only discovered
// instructions become protected code; unreferenced data is not scanned as code.
Bytes elf_image(const Image& image);
}
