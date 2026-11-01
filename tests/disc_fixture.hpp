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

inline nt::Bytes single_file_iso(const std::string& name,nt::ByteView payload) {
    nt::Bytes iso((21+(payload.size()+2047)/2048)*2048,0);
    const auto pvd=16*2048;
    iso[pvd]=1; const std::string signature="CD001";
    std::copy(signature.begin(),signature.end(),iso.begin()+pvd+1);
    iso[pvd+6]=1; iso[pvd+129]=8; iso[pvd+130]=8;
    both32(iso,pvd+80,static_cast<std::uint32_t>(iso.size()/2048));
    record(iso,pvd+156,std::string(1,'\0'),20,2048,true);
    record(iso,20*2048,name,21,static_cast<std::uint32_t>(payload.size()),false);
    std::copy(payload.begin(),payload.end(),iso.begin()+21*2048);
    return iso;
}

}
