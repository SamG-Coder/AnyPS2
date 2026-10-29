#pragma once
#include <array>
#include <cstdint>

namespace nt::sif {
// Public RPC_END bind reply shared by native EE and IOP service owners.
inline std::array<std::uint8_t,64> bind_reply(std::uint32_t rpc_id,std::uint32_t client,
                                            std::uint32_t server,std::uint32_t buffer) {
    std::array<std::uint8_t,64> result{};
    const auto put=[&](unsigned offset,std::uint32_t value) {
        for (unsigned byte=0;byte<4;++byte) result[offset+byte]=static_cast<std::uint8_t>(value>>(byte*8));
    };
    put(0,64); put(8,0x80000008); put(20,rpc_id); put(28,client);
    put(32,0x80000009); put(36,server); put(40,buffer);
    return result;
}
}
