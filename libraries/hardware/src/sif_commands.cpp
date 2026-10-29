#include <nt/sif.hpp>

namespace nt::hardware {
void Sif::initialize_commands() {
    if (commands_ready_) return;
    // This address is an allocation in our IOP HLE memory, not a retail BIOS address.
    registers_[1] = 0x1000;
    commands_ready_ = true;
    registers_[3] |= 0x30000; // SIF transport and command service initialized.
}
void Sif::dispatch_command(runtime::RamBus& ee) {
    const auto read = [&](unsigned offset) {
        if (offset + 4 > iop_packet_bytes_) throw runtime::MemoryFault("SIF command: truncated packet");
        std::uint32_t value = 0;
        for (unsigned byte = 0; byte < 4; ++byte)
            value |= std::uint32_t(iop_ram_[iop_packet_address_ + offset + byte]) << (byte * 8);
        return value;
    };
    const auto length = read(0) & 255;
    if (length < 16 || length > 112 || length > iop_packet_bytes_)
        throw runtime::MemoryFault("SIF command: invalid packet size");
    const auto command = read(8), option = read(12);
    const auto reply = [&](std::span<const std::uint32_t> words) {
        if (!replies_.empty()) throw runtime::MemoryFault("SIF RPC: reply queue busy");
        if (!ee_command_address_) throw runtime::MemoryFault("SIF RPC: EE receive address is unset");
        const auto bytes = (words.size() * 4 + 15) & ~std::size_t(15);
        std::vector<std::uint8_t> packet(bytes);
        for (unsigned i = 0; i < words.size(); ++i)
            for (unsigned byte = 0; byte < 4; ++byte) packet[i * 4 + byte] = static_cast<std::uint8_t>(words[i] >> (byte * 8));
        iop_install(0x1100,packet);
        iop_send(0x1100,ee_command_address_,static_cast<std::uint32_t>(bytes / 16));
    };
    if (command == 0x80000002U && option == 1) {
        // Acknowledge the dispatcher; services bind only when their backing exists.
        const std::array<std::uint32_t,6> response{24,0,0x80000001U,0,0,1};
        reply(response);
        rpc_ready_ = true;
        return;
    }
    if (command == 0x80000009U) {
        if (!rpc_ready_ || length < 36) throw runtime::MemoryFault("SIF RPC: invalid bind request");
        const bool available = fileio_ && read(32) == 0x80000001U;
        const std::array<std::uint32_t,12> response{48,0,0x80000008U,0,
            read(16),read(20),read(24),read(28),0x80000009U,
            available ? fileio_server : 0,available ? fileio_buffer : 0,0};
        reply(response);
        last_rpc_bind_server_ = read(32);
        ++rpc_bind_requests_;
        return;
    }
    if (command == 0x8000000aU) {
        if (!rpc_ready_ || length < 56) throw runtime::MemoryFault("SIF RPC: invalid call request");
        dispatch_rpc(ee,std::span(iop_ram_).subspan(iop_packet_address_,length));
        return;
    }
    if (command == 0x80000000U || command == 0x80000002U) {
        if (length < 20) throw runtime::MemoryFault("SIF command: missing EE buffer");
        if (command == 0x80000002U && option)
            throw runtime::MemoryFault("SIF command: unknown initialization option");
        const auto address = read(16);
        if (address % 16 || address >= 32 * 1024 * 1024 || !address)
            throw runtime::MemoryFault("SIF command: invalid EE buffer");
        ee_command_address_ = address;
        if (command == 0x80000002U) registers_[2] |= 0x20000;
    } else if (command == 0x80000001U) {
        if (length < 24 || read(16) >= command_registers_.size())
            throw runtime::MemoryFault("SIF command: invalid software register");
        command_registers_[read(16)] = read(20);
    } else throw runtime::MemoryFault("SIF command: unsupported command or RPC service");
}
}
