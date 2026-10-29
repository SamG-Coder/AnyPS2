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
};
Executable parse(ByteView bytes);
void describe(std::ostream& out, const Executable& executable);
}
