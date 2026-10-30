#pragma once
#include <nt/ee_rpc_service.hpp>
#include <nt/fileio.hpp>
#include <cstring>

namespace nt::ee {
class NativeFileRpc final : public NativeRpcService {
public:
    explicit NativeFileRpc(const std::filesystem::path& image)
        : NativeRpcService(0x80000001U,0x2000,0x3000),files_(image) {}
    fileio::Statistics statistics() const { return files_.statistics(); }
    Stop call(Memory& memory,CodeOverlap protection,std::uint32_t method,std::span<const std::uint8_t> args,
              std::uint32_t destination,std::uint32_t result_size) override {
        const auto writable=[&](std::uint32_t raw,std::uint32_t size) {
            const auto address=runtime::default_ram_address(raw);
            if (!memory.data || std::uint64_t(address)+size>memory.size || std::uint64_t(address)+size>0x2000000U) {
                memory.fault_address=raw; return Stop::memory_fault;
            }
            if (!protection) return Stop::invalid_state;
            if (size && protection(address,size)) { memory.fault_address=raw; return Stop::code_write; }
            return Stop::budget;
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
        fileio::Reply response;
        try { response=files_.call(method,args,result_size); }
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
