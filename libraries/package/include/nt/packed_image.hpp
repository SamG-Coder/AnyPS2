#pragma once
#include <nt/binary.hpp>
namespace nt::package {
struct Segment { std::uint32_t address; Bytes data; };
struct Image { std::uint32_t entry; std::vector<Segment> segments; };
enum class Content { ee, data, vu };
struct Region { std::uint32_t address,size; Content content; };
// Explicit offline classification, tied to the packed input's build identity.
// This validates a supplied declaration; it does not infer code from bytes.
std::vector<Region> parse_layout(ByteView text,std::uint64_t input_identity);
// Unencrypted, length-prefixed raw DEFLATE with bounded segmented-image payload.
// Opaque metadata is skipped, not authenticated. No filename/title selection.
Image unpack(ByteView input);
// Equal overlapping bytes are coalesced. Conflicting overlaps reject input.
// Without a layout, an AnyPS2 PT_NOTE import record preserves the container's
// lack of section permissions and allows
// static control-flow discovery in all file-backed segments. Only discovered
// instructions become protected code; unreferenced data is not scanned as code.
// A supplied layout must cover every loaded byte exactly once. It produces
// allocated EE/data/VU sections usable by whole-code conversion.
Bytes elf_image(const Image& image,std::span<const Region> layout={});
}
