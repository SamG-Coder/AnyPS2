#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace nt {
using Bytes = std::vector<std::uint8_t>;
using ByteView = std::span<const std::uint8_t>;
void require_range(ByteView bytes, std::size_t offset, std::size_t count);
std::uint16_t le16(ByteView bytes, std::size_t offset);
std::uint32_t le32(ByteView bytes, std::size_t offset);
std::uint32_t be32(ByteView bytes, std::size_t offset);
Bytes read_file(const std::filesystem::path& path, std::uint64_t limit = 128 * 1024 * 1024);
void write_file(const std::filesystem::path& path, ByteView bytes);
std::string hex32(std::uint32_t value);
// Non-cryptographic build identity check; not an authenticity guarantee.
std::uint64_t fingerprint(ByteView bytes);
}
