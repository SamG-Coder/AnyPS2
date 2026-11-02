#pragma once
#include <nt/ee_rpc_service.hpp>
#include <nt/fileio.hpp>
#include <cstring>

namespace nt::ee {
class NativeFileRpc final : public NativeRpcService {
public:
    explicit NativeFileRpc(const std::filesystem::path& image,fileio::ConsoleWriter output={})
        : NativeRpcService(0x80000001U,0x2000,0x3000),files_(image,std::move(output)) {}
    fileio::Statistics statistics() const { return files_.statistics(); }
    Stop call(Memory& memory,CodeOverlap protection,std::uint32_t method,std::span<const std::uint8_t> args,
              std::uint32_t destination,std::uint32_t result_size) override {
        const auto writable=[&](std::uint32_t raw,std::uint32_t size) {
            return rpc_output(memory,protection,raw,size);
        };
        auto valid=writable(destination,result_size);
        if (valid!=Stop::budget) return valid;
        // Reads also write the destination and optional unaligned-edge fixup.
        // Check their full requested extents before advancing a disc handle.
        if (method==2) {
            if (args.size()<16 || le32(args,8)>0x100000) return Stop::invalid_state;
            valid=writable(le32(args,4),le32(args,8));
            if (valid!=Stop::budget) return valid;
            if (const auto fixup=le32(args,12)) {
                valid=writable(fixup,16);
                if (valid!=Stop::budget) return valid;
            }
        }
        Bytes source;
        if (method==3) {
            if (args.size()!=32 || result_size!=4) return Stop::invalid_state;
            const auto raw=le32(args,4),size=le32(args,8),prefix=le32(args,12);
            if (size>0x100000 || prefix>16 || prefix>size) return Stop::invalid_state;
            const auto address=runtime::default_ram_address(raw);
            if (!memory.data || std::uint64_t(address)+size>memory.size || std::uint64_t(address)+size>0x2000000U) {
                memory.fault_address=raw; return Stop::memory_fault;
            }
            source.assign(args.begin()+16,args.begin()+16+prefix);
            source.insert(source.end(),memory.data+address+prefix,memory.data+address+size);
        }
        fileio::Reply response;
        try { response=files_.call(method,args,result_size,source); }
        catch (const runtime::MemoryFault&) { return Stop::unsupported_system; }
        // Service output ranges were checked above. Byte copies intentionally
        // preserve adjacent bytes even for unaligned reads and short EOF results.
        for (const auto& write:response.writes)
            if (!write.bytes.empty()) std::memcpy(memory.data+runtime::default_ram_address(write.address),write.bytes.data(),write.bytes.size());
        if (!response.result.empty()) std::memcpy(memory.data+runtime::default_ram_address(destination),response.result.data(),response.result.size());
        return Stop::budget;
    }
private:
    fileio::Service files_;
};
}
