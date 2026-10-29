#include <nt/elf.hpp>
#include <algorithm>
#include <stdexcept>

namespace nt::elf {
Executable parse(ByteView b) {
    require_range(b, 0, 52);
    constexpr std::uint8_t magic[] = {0x7f, 'E', 'L', 'F', 1, 1, 1};
    if (!std::equal(std::begin(magic), std::end(magic), b.begin()))
        throw std::runtime_error("Expected little-endian ELF32");
    if (le16(b, 16) != 2 || le16(b, 18) != 8 || le32(b, 20) != 1 || le16(b, 40) != 52)
        throw std::runtime_error("Expected MIPS ET_EXEC ELF");
    Executable result{le32(b, 24), le32(b, 36), {}};
    const auto table = le32(b, 28);
    const auto stride = le16(b, 42), count = le16(b, 44);
    if (stride < 32 || count == 0) throw std::runtime_error("Invalid ELF program headers");
    require_range(b, table, std::size_t(stride) * count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto p = table + i * stride;
        if (le32(b, p) != 1) continue;
        Segment s{le32(b, p + 4), le32(b, p + 8), le32(b, p + 16),
                  le32(b, p + 20), le32(b, p + 24), le32(b, p + 28)};
        require_range(b, s.offset, s.file_size);
        if (s.file_size > s.memory_size || std::uint64_t(s.address) + s.memory_size > 0x100000000ULL)
            throw std::runtime_error("Invalid ELF segment extent");
        if (s.alignment > 1 && ((s.alignment & (s.alignment - 1)) ||
            (s.address % s.alignment != s.offset % s.alignment)))
            throw std::runtime_error("Invalid ELF segment alignment");
        result.segments.push_back(s);
    }
    const auto contains_entry = [&](const Segment& s) {
        return (s.flags & 1) && result.entry >= s.address &&
               std::uint64_t(result.entry) + 4 <= std::uint64_t(s.address) + s.file_size;
    };
    if (result.entry % 4 || !std::any_of(result.segments.begin(), result.segments.end(), contains_entry))
        throw std::runtime_error("Entry is not aligned, file-backed executable memory");
    return result;
}
void describe(std::ostream& out, const Executable& executable) {
    out << "MIPS ELF32 entry " << hex32(executable.entry) << " flags " << hex32(executable.flags) << '\n';
    for (const auto& s : executable.segments)
        out << "  LOAD " << hex32(s.address) << " file=" << s.file_size << " memory=" << s.memory_size
            << " flags=" << s.flags << " offset=" << s.offset << '\n';
    out << "Import metadata only; native translation has not been performed.\n";
}
}
