#include <nt/fileio.hpp>
#include <algorithm>
#include <bit>

namespace nt::fileio {
namespace {
void validate_write(std::uint32_t address, std::uint32_t size) {
    address = runtime::default_ram_address(address);
    if (address > 0x2000000 || size > 0x2000000 - address)
        throw runtime::MemoryFault("FILEIO: EE destination exceeds RAM");
}
}
Reply Service::call(std::uint32_t method, ByteView args, std::uint32_t result_size) {
    Reply reply;
    ++statistics_.requests; statistics_.last_method = method;
    if (method == 255) {
        if (result_size != 4 && result_size != 8) throw runtime::MemoryFault("FILEIO: invalid version result size");
        reply.result = {'1','0','0','0','.','.','.','.'};
        reply.result.resize(result_size);
        return reply;
    }
    if (result_size != 4) throw runtime::MemoryFault("FILEIO: expected a four-byte result");
    const auto argument = [&](unsigned index) {
        if (index * 4 + 4 > args.size()) throw runtime::MemoryFault("FILEIO: truncated arguments");
        return le32(args,index * 4);
    };
    std::int32_t result = -1;
    if (method == 0) {
        const auto mode = argument(0);
        if (args.size() < 5 || args.size() > 272) throw runtime::MemoryFault("FILEIO: invalid open arguments");
        const auto end = std::find(args.begin()+4,args.end(),0);
        if (end == args.end() || end - (args.begin()+4) >= 256) throw runtime::MemoryFault("FILEIO: unterminated path");
        std::string path(args.begin()+4,end);
        if (mode != 1) result = -30; // Read-only filesystem.
        else if (!path.starts_with("cdrom0:") && !path.starts_with("cdrom:")) result = -19;
        else {
            path.erase(0,path.find(':')+1);
            // ISO resolution rejects traversal; it never resolves host paths.
            disc::Entry entry;
            try { entry = disc_.find(path); }
            catch (const disc::PathError&) { result = -2; }
            if (!entry.name.empty()) {
                if (entry.directory) result = -21;
                else if (handles_.size() >= 64) result = -24;
                else {
                    std::uint32_t fd = 3;
                    while (handles_.contains(fd)) ++fd;
                    handles_.emplace(fd,Handle{entry}); result = static_cast<std::int32_t>(fd);
                    ++statistics_.opens;
                }
            }
        }
    } else if (method == 1 || method == 2 || method == 4) {
        const auto fd = argument(0);
        const auto found = handles_.find(fd);
        if (found == handles_.end()) result = -9;
        else if (method == 1) { handles_.erase(found); result = 0; }
        else if (method == 4) {
            const auto offset = std::bit_cast<std::int32_t>(argument(1));
            const auto whence = argument(2);
            const auto& handle = found->second;
            const auto base = whence == 0 ? 0LL : whence == 1 ? std::int64_t(handle.offset) : std::int64_t(handle.entry.size);
            const auto position = base + offset;
            if (whence > 2 || position < 0 || position > 0x7fffffff) result = -22;
            else { found->second.offset = static_cast<std::uint32_t>(position); result = static_cast<std::int32_t>(position); }
        } else {
            const auto destination = argument(1), size = argument(2), fixup = argument(3);
            if (size > 0x100000) throw runtime::MemoryFault("FILEIO: read exceeds bounded transfer size");
            validate_write(destination,size);
            if (fixup) validate_write(fixup,16);
            auto& handle = found->second;
            const auto count = handle.offset >= handle.entry.size ? 0U : std::min(size,handle.entry.size-handle.offset);
            if (count) reply.writes.push_back({destination,disc_.read_range(handle.entry,handle.offset,count)});
            // HLE sends every requested byte, including unaligned edges. The guest
            // read callback therefore has no prefix/suffix fragments to copy.
            if (fixup) reply.writes.push_back({fixup,Bytes(16,0)});
            handle.offset += count; result = static_cast<std::int32_t>(count);
            statistics_.bytes_read += count;
        }
    } else if (method == 3 || method == 6 || method == 7 || method == 8 || method == 13 || method == 14) result = -30;
    else throw runtime::MemoryFault("FILEIO: unsupported method " + std::to_string(method));
    reply.result.resize(4);
    for (unsigned i=0;i<4;++i) reply.result[i] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(result) >> (8*i));
    return reply;
}
}
