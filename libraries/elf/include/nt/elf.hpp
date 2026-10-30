#pragma once
#include <nt/binary.hpp>
#include <ostream>

namespace nt::elf {
struct Segment {
    std::uint32_t offset, address, file_size, memory_size, flags, alignment;
};
struct Executable {
    std::uint32_t entry, flags;
    std::vector<Segment> segments;
    // Metadata roots only; application startup retains invocation order.
    std::vector<std::uint32_t> lifecycle_entries;
};
Executable parse(ByteView bytes);
void describe(std::ostream& out, const Executable& executable);
}
