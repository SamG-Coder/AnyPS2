#pragma once
#include <nt/ee_handlers.hpp>
#include <span>

namespace nt::ee {
inline Stop rpc_output(Memory& memory,CodeOverlap protection,std::uint32_t raw,std::uint32_t size) {
    const auto address=runtime::default_ram_address(raw);
    if (!memory.data || std::uint64_t(address)+size>memory.size || std::uint64_t(address)+size>0x2000000U) {
        memory.fault_address=raw; return Stop::memory_fault;
    }
    if (!protection) return Stop::invalid_state;
    if (size && protection(address,size)) { memory.fault_address=raw; return Stop::code_write; }
    return Stop::budget;
}
// Native service tokens identify owned endpoints, not executable IOP addresses.
class NativeRpcService {
public:
    NativeRpcService(std::uint32_t id,std::uint32_t server,std::uint32_t buffer)
        : id(id),server(server),buffer(buffer) {}
    virtual ~NativeRpcService()=default;
    const std::uint32_t id,server,buffer;
    // Validate outputs before changing service state. On success, publish all
    // result bytes; the transport posts the completion packet afterwards.
    virtual Stop call(Memory&,CodeOverlap,std::uint32_t method,std::span<const std::uint8_t> arguments,
                      std::uint32_t destination,std::uint32_t result_size)=0;
};
}
