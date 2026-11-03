#include <nt/vu_upload.hpp>
#include <algorithm>
#include <stdexcept>
#include <array>
#include <map>

namespace nt::vu {
std::vector<DeclaredUploadImage> recover_declared_uploads(ByteView file,std::span<const elf::VectorOverlay> overlays) {
    std::vector<DeclaredUploadImage> result;
    std::optional<elf::VectorOverlay> previous;
    for (const auto& overlay:overlays) {
        require_range(file,overlay.file_offset,overlay.size);
        if (!overlay.size || overlay.size%8 || overlay.destination%8 ||
            std::uint64_t(overlay.destination)+overlay.size>16384)
            throw std::invalid_argument("Invalid declared VU upload range");
        bool adjacent=false;
        if (previous && previous->destination+previous->size==overlay.destination) {
            const auto end=std::uint64_t(previous->file_offset)+previous->size;
            const auto source_end=std::uint64_t(previous->source_address)+previous->size;
            if (overlay.file_offset>=end+4 && overlay.file_offset-end<=4096 &&
                end%4==0 && overlay.file_offset%4==0 && overlay.source_address>=source_end &&
                overlay.source_address-source_end==overlay.file_offset-end) {
                const auto command=le32(file,overlay.file_offset-4);
                const auto count=(command>>16)&255U;
                adjacent=(command>>24)==0x4a && (command&65535U)==overlay.destination/8 &&
                    (count ? count : 256U)*8==overlay.size;
                for (auto at=end;adjacent && at<overlay.file_offset-4;at+=4)
                    adjacent=le32(file,static_cast<std::size_t>(at))==0;
            }
        }
        if (!adjacent) result.push_back({{overlay.destination/8,{}},{}});
        auto& group=result.back();
        const auto payload=file.subspan(overlay.file_offset,overlay.size);
        group.image.bytes.insert(group.image.bytes.end(),payload.begin(),payload.end());
        group.sources.push_back(overlay.source_address);
        previous=overlay;
    }
    return result;
}
std::vector<Upload> assemble_uploads(std::span<const Upload> uploads,unsigned unit) {
    if (unit>1) throw std::invalid_argument("Invalid VU unit");
    std::map<unsigned,std::array<std::uint8_t,8>> image;
    for (const auto& upload:uploads) {
        if (upload.bytes.empty() || upload.bytes.size()%8 ||
            std::uint64_t(upload.destination)+upload.bytes.size()/8>(unit ? 2048U : 512U))
            throw std::invalid_argument("Invalid VU upload extent");
        for (std::size_t offset=0;offset<upload.bytes.size();offset+=8) {
            auto& pair=image[upload.destination+static_cast<unsigned>(offset/8)];
            std::copy_n(upload.bytes.begin()+offset,8,pair.begin());
        }
    }
    std::vector<Upload> regions;
    for (const auto& [address,bytes]:image) {
        if (regions.empty() || regions.back().destination+regions.back().bytes.size()/8!=address)
            regions.push_back({address,{}});
        regions.back().bytes.insert(regions.back().bytes.end(),bytes.begin(),bytes.end());
    }
    return regions;
}
UploadSequence extract_uploads(ByteView packet,unsigned unit) {
    if (unit>1 || packet.size()%4 || packet.size()>0x100000)
        throw std::invalid_argument("Invalid VIF upload packet");
    UploadSequence result;
    while (result.consumed<packet.size()) {
        const auto offset=result.consumed;
        std::uint32_t code=0;
        for (unsigned byte=0;byte<4;++byte) code|=std::uint32_t(packet[offset+byte])<<(byte*8);
        const auto command=(code>>24)&127U;
        std::size_t payload=0;
        if (code&0x80000000U) { result.stopped_command=command; break; }
        switch (command) {
        case 0: case 1: case 5: case 7: case 0x10: break;
        case 0x20: payload=4; break;
        case 0x30: case 0x31: payload=16; break;
        case 0x4a: payload=(((code>>16)&255U) ? ((code>>16)&255U) : 256U)*8U; break;
        default: result.stopped_command=command; return result;
        }
        if (packet.size()-offset-4<payload) throw std::invalid_argument("Truncated VIF upload packet");
        if (command==0x4a) {
            const auto destination=code&0xffffU;
            if (destination+payload/8>(unit ? 2048U : 512U))
                throw std::invalid_argument("VU upload wraps or exceeds unit capacity");
            const auto bytes=packet.subspan(offset+4,payload);
            result.uploads.push_back({destination,Bytes(bytes.begin(),bytes.end())});
        }
        result.consumed+=4+payload;
    }
    return result;
}
}
