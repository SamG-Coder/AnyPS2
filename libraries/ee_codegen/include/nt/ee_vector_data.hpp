#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <optional>
#include <algorithm>
#include <cstring>
#include <vector>

namespace nt::ee {
enum class NativeVectorFormat : unsigned { words, halfwords, bytes, packed5551 };
inline unsigned vector_data_stride(NativeVectorFormat format,unsigned components=4) {
    if (!components || components>4) return 0;
    switch (format) {
    case NativeVectorFormat::words: return components*4;
    case NativeVectorFormat::halfwords: return components*2;
    case NativeVectorFormat::bytes: return components;
    case NativeVectorFormat::packed5551: return components==4 ? 2 : 0;
    }
    return 0;
}
struct NativeVectorMask {
    std::uint32_t selectors=0;
    std::array<std::uint32_t,8> row_column{};
    unsigned cycle=256;
};
struct NativeVectorWrite {
    unsigned destination,lanes=15;
    std::array<std::uint32_t,4> values;
};
struct NativeVectorPlan {
    std::vector<NativeVectorWrite> writes;
    std::array<std::uint32_t,4> rows;
};
struct NativeVectorDataView {
    unsigned destination;
    std::span<const std::uint8_t> bytes;
    NativeVectorFormat format=NativeVectorFormat::words;
    bool unsigned_values=false;
    unsigned components=4;
    std::optional<NativeVectorMask> mask;
    std::array<std::uint32_t,4> rows{};
    unsigned mode=0;
    std::span<const NativeVectorWrite> planned;
    unsigned destination_cycle=256,writes_per_cycle=256;
    unsigned output_count=0;
    unsigned source_alignment=0; // Payload word position 1..4 in the source quadword; zero means unknown.
    std::span<const std::uint8_t> following_bytes;
};
// Plan bounded data writes and final row values without touching destination storage.
inline std::optional<NativeVectorPlan> plan_vector_data(std::size_t storage_size,NativeVectorDataView upload) {
    const auto stride=vector_data_stride(upload.format,upload.components);
    if (!storage_size || upload.destination>=storage_size || upload.mode>3 || !stride ||
        upload.bytes.size()%stride || upload.bytes.size()/stride>256 || upload.source_alignment>4) return {};
    if (upload.mask && (!upload.mask->cycle || upload.mask->cycle>256)) return {};
    if (!upload.writes_per_cycle || upload.writes_per_cycle>256 || upload.destination_cycle>256) return {};
    const bool filling=upload.writes_per_cycle>upload.destination_cycle;
    const auto outputs=upload.output_count ? upload.output_count : unsigned(upload.bytes.size()/stride);
    if (!outputs || outputs>256) return {};
    const auto inputs=filling ? (outputs/upload.writes_per_cycle)*upload.destination_cycle+
        std::min(outputs%upload.writes_per_cycle,upload.destination_cycle) : outputs;
    if (upload.bytes.size()!=inputs*stride) return {};
    NativeVectorPlan plan{{},upload.rows};
    if (upload.mask) std::copy_n(upload.mask->row_column.begin(),4,plan.rows.begin());
    plan.writes.reserve(outputs);
    // Little-endian guest data on a little-endian host: a plain load.
    const auto read=[&](std::size_t offset,unsigned width) {
        std::uint32_t value=0;
        std::memcpy(&value,upload.bytes.data()+offset,width);
        return value;
    };
    // Unmasked 32-bit four-component data copies whole vectors (with the
    // row-register arithmetic of modes 1-3 applied per lane).
    if (!upload.mask && !filling && upload.format==NativeVectorFormat::words && upload.components==4) {
        plan.writes.resize(outputs);
        for (unsigned index=0;index<outputs;++index) {
            const auto within=index%upload.writes_per_cycle;
            const auto displacement=(index/upload.writes_per_cycle)*upload.destination_cycle+within;
            auto& write=plan.writes[index];
            write.destination=unsigned((upload.destination+displacement)%storage_size); write.lanes=15;
            std::memcpy(write.values.data(),upload.bytes.data()+std::size_t(index)*16,16);
            if (upload.mode) for (unsigned lane=0;lane<4;++lane) {
                if (upload.mode==1 || upload.mode==2) write.values[lane]+=plan.rows[lane];
                if (upload.mode==2 || upload.mode==3) plan.rows[lane]=write.values[lane];
            }
        }
        return plan;
    }
    for (unsigned index=0;index<outputs;++index) {
        const auto within=index%upload.writes_per_cycle;
        const bool fill=filling && within>=upload.destination_cycle;
        const auto source_index=filling ? (index/upload.writes_per_cycle)*upload.destination_cycle+within : index;
        const auto offset=std::size_t(source_index)*stride;
        std::array<std::uint32_t,4> vector{};
        if (!fill && upload.format==NativeVectorFormat::packed5551) {
            const auto packed=read(offset,2);
            for (unsigned lane=0;lane<3;++lane) vector[lane]=((packed>>(lane*5))&31U)*8U;
            vector[3]=(packed>>15)*128U;
        } else if (!fill) {
            const auto width=stride/upload.components;
            for (unsigned lane=0;lane<4;++lane) {
                if (upload.components==3 && lane==3) continue;
                auto value=read(offset+(lane%upload.components)*width,width);
                if (width<4 && !upload.unsigned_values && (value&(1U<<(width*8-1)))) value|=~0U<<(width*8);
                vector[lane]=value;
            }
        }
        const auto displacement=filling ? index : (index/upload.writes_per_cycle)*upload.destination_cycle+within;
        NativeVectorWrite write{unsigned((upload.destination+displacement)%storage_size),15,vector};
        const auto group=upload.mask ? std::min(unsigned(index)%upload.mask->cycle,3U) : 0U;
        for (unsigned lane=0;lane<4;++lane) {
            const auto selection=upload.mask ? (upload.mask->selectors>>(group*8+lane*2))&3U : 0U;
            if (fill && !selection) return {}; // Input-selected fill values require unmodeled lookahead.
            if (upload.components==3 && lane==3 && !selection) {
                if (!upload.source_alignment) return {};
                const auto width=stride/3;
                const unsigned step=source_index+1;
                // V3's fourth component follows the source unpack alignment,
                // not an invented homogeneous coordinate or a retained W lane.
                const bool peek=width==4 ? (source_index%2)==upload.source_alignment :
                    width==2 ? !(step%2==0 && ((step/4+5-upload.source_alignment)%4)==0) :
                    step==upload.source_alignment;
                auto value=0U;
                if (peek) {
                    const auto start=offset+stride;
                    if (start+width>upload.bytes.size() && start+width-upload.bytes.size()>upload.following_bytes.size()) return {};
                    for (unsigned byte=0;byte<width;++byte) {
                        const auto position=start+byte;
                        const auto part=position<upload.bytes.size() ? upload.bytes[position] : upload.following_bytes[position-upload.bytes.size()];
                        value|=std::uint32_t(part)<<(byte*8);
                    }
                    if (width<4 && !upload.unsigned_values && (value&(1U<<(width*8-1)))) value|=~0U<<(width*8);
                }
                write.values[lane]=value;
            }
            if (selection==1) write.values[lane]=plan.rows[lane];
            else if (selection==2) write.values[lane]=upload.mask->row_column[4+group];
            else if (selection==3) write.lanes&=~(1U<<lane);
            else if (upload.format!=NativeVectorFormat::packed5551) {
                if (upload.mode==1 || upload.mode==2) write.values[lane]+=plan.rows[lane];
                if (upload.mode==2 || upload.mode==3) plan.rows[lane]=write.values[lane];
            }
        }
        plan.writes.push_back(write);
    }
    return plan;
}
inline bool apply_vector_writes(std::span<std::array<std::uint32_t,4>> storage,std::span<const NativeVectorWrite> writes) {
    if (writes.empty() || writes.size()>256) return false;
    for (const auto& write:writes) if (write.destination>=storage.size() || write.lanes>15) return false;
    for (const auto& write:writes) for (unsigned lane=0;lane<4;++lane)
        if (write.lanes&(1U<<lane)) storage[write.destination][lane]=write.values[lane];
    return true;
}
inline bool write_vector_data(std::span<std::array<std::uint32_t,4>> storage,NativeVectorDataView upload) {
    if (!upload.planned.empty()) return apply_vector_writes(storage,upload.planned);
    const auto plan=plan_vector_data(storage.size(),upload);
    return plan && apply_vector_writes(storage,plan->writes);
}
}
