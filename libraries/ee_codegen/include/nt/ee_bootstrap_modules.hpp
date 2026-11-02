#pragma once
#include <nt/ee_rpc_service.hpp>
#include <nt/binary.hpp>
#include <algorithm>
#include <array>
#include <cstring>

namespace nt::ee {
// Standalone EE bootstrap has no resident IOP modules and no mounted ROM. Its
// public module-list export is an empty record, not a fabricated firmware image.
// A compiled IOP reset hands traffic to the existing module lifecycle owner.
class NativeBootstrapModules final : public NativeRpcService {
public:
    NativeBootstrapModules() : NativeRpcService(0x80000006U,0x8000,0x9000) {}
    Stop snapshot(std::uint32_t address,std::span<std::uint8_t> output) const {
        address=runtime::default_ram_address(address);
        constexpr std::uint32_t list=0x800,record_bytes=48;
        if (address<list || std::uint64_t(address)+output.size()>list+record_bytes)
            return Stop::unsupported_system;
        std::fill(output.begin(),output.end(),std::uint8_t{0});
        return Stop::budget;
    }
    Stop call(Memory& memory,CodeOverlap protection,std::uint32_t method,std::span<const std::uint8_t> args,
              std::uint32_t destination,std::uint32_t result_size) override {
        const auto valid=rpc_output(memory,protection,destination,result_size);
        if (valid!=Stop::budget) return valid;
        if (method!=0) return Stop::unsupported_system;
        if (args.size()!=512 || result_size!=8 || le32(args,0)>252) return Stop::invalid_state;
        const auto path=args.subspan(8,252);
        const auto end=std::find(path.begin(),path.end(),0);
        if (end==path.end()) return Stop::invalid_state;
        // Only an absent ROM namespace is known here. Disc/host module loads
        // require a sealed AOT catalog and may not become successful stubs.
        constexpr std::array<std::uint8_t,5> rom{'r','o','m','0',':'};
        if (end-path.begin()<6 || !std::equal(rom.begin(),rom.end(),path.begin())) return Stop::unsupported_system;
        const std::array<std::uint32_t,2> failure{static_cast<std::uint32_t>(-203),0}; // KE_NOFILE
        std::memcpy(memory.data+runtime::default_ram_address(destination),failure.data(),sizeof(failure));
        return Stop::budget;
    }
};
}
