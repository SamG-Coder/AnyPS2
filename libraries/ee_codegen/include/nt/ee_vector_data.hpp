#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace nt::ee {
enum class NativeVectorFormat : unsigned { words, halfwords, bytes, packed5551 };
inline unsigned vector_data_stride(NativeVectorFormat format) {
    switch (format) {
    case NativeVectorFormat::words: return 16;
    case NativeVectorFormat::halfwords: return 8;
    case NativeVectorFormat::bytes: return 4;
    case NativeVectorFormat::packed5551: return 2;
    }
    return 0;
}
struct NativeVectorDataView {
    unsigned destination;
    std::span<const std::uint8_t> bytes;
    NativeVectorFormat format=NativeVectorFormat::words;
    bool unsigned_values=false;
};
// Convert transport data into native vector storage. Input contains no program
// instructions. Validate the complete view before changing any destination.
inline bool write_vector_data(std::span<std::array<std::uint32_t,4>> storage,NativeVectorDataView upload) {
    const auto stride=vector_data_stride(upload.format);
    if (storage.empty() || upload.destination>=storage.size() || !stride || upload.bytes.empty() ||
        upload.bytes.size()%stride || upload.bytes.size()/stride>256) return false;
    const auto read=[&](std::size_t offset,unsigned width) {
        std::uint32_t value=0;
        for (unsigned byte=0;byte<width;++byte) value|=std::uint32_t(upload.bytes[offset+byte])<<(byte*8);
        return value;
    };
    for (std::size_t offset=0;offset<upload.bytes.size();offset+=stride) {
        std::array<std::uint32_t,4> vector;
        if (upload.format==NativeVectorFormat::packed5551) {
            const auto packed=read(offset,2);
            for (unsigned lane=0;lane<3;++lane) vector[lane]=((packed>>(lane*5))&31U)*8U;
            vector[3]=(packed>>15)*128U;
        } else {
            const auto width=stride/4;
            for (unsigned lane=0;lane<4;++lane) {
                auto value=read(offset+lane*width,width);
                if (width<4 && !upload.unsigned_values && (value&(1U<<(width*8-1)))) value|=~0U<<(width*8);
                vector[lane]=value;
            }
        }
        storage[(upload.destination+offset/stride)%storage.size()]=vector;
    }
    return true;
}
}
