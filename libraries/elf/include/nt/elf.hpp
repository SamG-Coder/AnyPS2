#pragma once
#include <nt/binary.hpp>
#include <ostream>

namespace nt::elf {
struct Segment {
    std::uint32_t offset, address, file_size, memory_size, flags, alignment;
};
struct AddressRange { std::uint32_t address,size; };
struct Executable {
    std::uint32_t entry, flags;
    std::vector<Segment> segments;
    // Metadata roots only; application startup retains invocation order.
    std::vector<std::uint32_t> lifecycle_entries;
    // An AnyPS2 import note records that the source container had no section
    // permissions. Only discovered flow establishes code in such an image.
    bool inferred_permissions=false;
    // Allocated section metadata distinguishes non-EE contents inside a mixed LOAD.
    std::vector<AddressRange> non_executable_sections;
    // VU payload containers remain in the load image but are not EE functions.
    // Their upload framing and VU entry points still require separate conversion.
    std::vector<AddressRange> vector_sections;
    // Defined STT_FUNC symbols validated against file-backed executable sections.
    // Conversion may opt into these roots without promoting object/data symbols.
    std::vector<std::uint32_t> function_entries;
};
Executable parse(ByteView bytes);
bool executable_address(const Executable&,std::uint32_t address,unsigned bytes=4,bool allow_inferred=false);
void describe(std::ostream& out, const Executable& executable);
}
