#pragma once
#include <nt/ee_handlers.hpp>
#include <span>

namespace nt::ee {
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
