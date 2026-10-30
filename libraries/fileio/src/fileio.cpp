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
Bytes Service::read_disc_file(const std::string& guest_path, std::uint64_t limit) {
    try { return files_.read_file(guest_path,limit); }
    catch (const disc::PathError& error) { throw runtime::MemoryFault(error.what()); }
}
Reply Service::call(std::uint32_t method, ByteView args, std::uint32_t result_size) {
    Reply reply;
    if (method == 255) {
        if (result_size != 4 && result_size != 8) throw runtime::MemoryFault("FILEIO: invalid version result size");
        reply.result = {'1','0','0','0','.','.','.','.'};
        reply.result.resize(result_size);
        ++statistics_.requests; statistics_.last_method = method;
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
        result=files_.open(path,mode);
        if (result>=0) ++statistics_.opens;
    } else if (method == 1 || method == 2 || method == 4) {
        const auto fd = argument(0);
        if (!files_.contains(fd)) result=-9;
        else if (method==1) result=files_.close(fd);
        else if (method==4) {
            result=files_.seek(fd,std::bit_cast<std::int32_t>(argument(1)),argument(2));
        } else {
            const auto destination = argument(1), size = argument(2), fixup = argument(3);
            if (size > 0x100000) throw runtime::MemoryFault("FILEIO: read exceeds bounded transfer size");
            validate_write(destination,size);
            if (fixup) validate_write(fixup,16);
            auto data=files_.read(fd,size);
            const auto count=static_cast<std::uint32_t>(data.result);
            if (count) reply.writes.push_back({destination,std::move(data.bytes)});
            // HLE sends every requested byte, including unaligned edges. The guest
            // read callback therefore has no prefix/suffix fragments to copy.
            if (fixup) reply.writes.push_back({fixup,Bytes(16,0)});
            result = static_cast<std::int32_t>(count);
            statistics_.bytes_read += count;
        }
    } else if (method == 3 || method == 6 || method == 7 || method == 8 || method == 13 || method == 14) result = -30;
    else throw runtime::MemoryFault("FILEIO: unsupported method " + std::to_string(method));
    reply.result.resize(4);
    for (unsigned i=0;i<4;++i) reply.result[i] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(result) >> (8*i));
    ++statistics_.requests; statistics_.last_method = method;
    return reply;
}
}
