#pragma once
#include <nt/binary.hpp>
#include <algorithm>
namespace fixture {
inline void put32(nt::Bytes& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.at(offset + i) = static_cast<std::uint8_t>(value >> (i * 8));
}
inline void both32(nt::Bytes& bytes, std::size_t offset, std::uint32_t value) {
    put32(bytes, offset, value);
    for (unsigned i = 0; i < 4; ++i) bytes.at(offset + 4 + i) = static_cast<std::uint8_t>(value >> ((3 - i) * 8));
}
inline std::size_t record(nt::Bytes& b, std::size_t offset, const std::string& name,
                   std::uint32_t sector, std::uint32_t size, bool directory) {
    const auto length = 33 + name.size() + (name.size() % 2 == 0 ? 1 : 0);
    b.at(offset) = static_cast<std::uint8_t>(length);
    both32(b, offset + 2, sector); both32(b, offset + 10, size);
    b.at(offset + 25) = directory ? 2 : 0;
    b.at(offset + 32) = static_cast<std::uint8_t>(name.size());
    std::copy(name.begin(), name.end(), b.begin() + offset + 33);
    return length;
}

}
