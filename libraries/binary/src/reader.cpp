#include <nt/binary.hpp>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace nt {
std::uint64_t fingerprint(ByteView bytes) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (auto byte : bytes) { hash ^= byte; hash *= 1099511628211ULL; }
    return hash;
}
void require_range(ByteView b, std::size_t offset, std::size_t count) {
    if (offset > b.size() || count > b.size() - offset)
        throw std::runtime_error("Truncated binary structure");
}
std::uint16_t le16(ByteView b, std::size_t p) {
    require_range(b, p, 2);
    return static_cast<std::uint16_t>(b[p] | (b[p + 1] << 8));
}
std::uint32_t le32(ByteView b, std::size_t p) {
    require_range(b, p, 4);
    return std::uint32_t(b[p]) | (std::uint32_t(b[p + 1]) << 8) |
           (std::uint32_t(b[p + 2]) << 16) | (std::uint32_t(b[p + 3]) << 24);
}
std::uint32_t be32(ByteView b, std::size_t p) {
    require_range(b, p, 4);
    return (std::uint32_t(b[p]) << 24) | (std::uint32_t(b[p + 1]) << 16) |
           (std::uint32_t(b[p + 2]) << 8) | std::uint32_t(b[p + 3]);
}
Bytes read_file(const std::filesystem::path& path, std::uint64_t limit) {
    const auto size = std::filesystem::file_size(path);
    if (size > limit) throw std::runtime_error("File exceeds read limit");
    Bytes bytes(static_cast<std::size_t>(size));
    std::ifstream file(path, std::ios::binary);
    if (!file || !file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
        throw std::runtime_error("Cannot read input file");
    return bytes;
}
void write_file(const std::filesystem::path& path, ByteView bytes) {
    std::ofstream file(path, std::ios::binary);
    if (!file || !file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("Cannot write output file");
    file.close();
    if (!file) throw std::runtime_error("Cannot finish output file");
}
void write_text_file(const std::filesystem::path& path, std::string_view text) {
    write_file(path, ByteView(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}
std::string hex32(std::uint32_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::setw(8) << std::setfill('0') << value;
    return out.str();
}
}
