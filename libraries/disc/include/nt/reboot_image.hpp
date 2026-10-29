#pragma once
#include <nt/binary.hpp>

namespace nt::disc {
struct RebootFile {
    std::string name;
    std::size_t offset, size, info_offset, info_size;
};
// Disc module archives with an empty RESET entry, not firmware ROM images.
// Offsets refer to the caller-owned image; no guest code is executed by parsing.
std::vector<RebootFile> parse_reboot_image(ByteView image);
}
