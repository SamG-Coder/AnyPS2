#include <nt/sif.hpp>
#include <algorithm>

namespace nt::hardware {
void Sif::mount_disc(const std::filesystem::path& image) {
    if (commands_ready_ || fileio_) throw runtime::MemoryFault("SIF: mount disc before initializing commands");
    fileio_ = std::make_unique<fileio::Service>(image);
}
void Sif::dispatch_rpc(runtime::RamBus& ee, std::span<const std::uint8_t> packet) {
    const auto read = [&](unsigned offset) { return le32(packet,offset); };
    const auto method = read(32), send_size = read(36), destination = read(40), result_size = read(44);
    if (!fileio_ || read(52) != fileio_server) throw runtime::MemoryFault("SIF RPC: unregistered server");
    if (send_size > 4096 || (result_size != 4 && result_size != 8) || read(48) != 1)
        throw runtime::MemoryFault("SIF RPC: unsupported FILEIO call sizes or response mode");
    const auto physical = runtime::default_ram_address(destination);
    if (physical > 0x2000000 || result_size > 0x2000000-physical)
        throw runtime::MemoryFault("SIF RPC: result exceeds EE RAM");
    if (transmit_.active || !replies_.empty()) throw runtime::MemoryFault("SIF RPC: reply queue busy");
    auto reply = fileio_->call(method,std::span(iop_ram_).subspan(fileio_buffer,send_size),result_size);
    reply.writes.push_back({destination,std::move(reply.result)});
    const std::array<std::uint32_t,12> response{48,0,0x80000008U,0,
        read(16),read(20),read(24),read(28),0x8000000aU,0,0,0};
    queue_rpc_reply(ee,std::move(reply.writes),response);
}
void Sif::queue_rpc_reply(runtime::RamBus& ee, std::vector<fileio::Write> writes, std::span<const std::uint32_t> response) {
    if (!ee_command_address_) throw runtime::MemoryFault("SIF RPC: missing EE command address");
    // Merge partial quadwords before DMA so adjacent/overlapping outputs preserve
    // untouched bytes. Only the final command packet terminates the DMA chain.
    std::map<std::uint32_t,std::array<std::uint8_t,16>> quads;
    for (const auto& write : writes) {
        const auto address = runtime::default_ram_address(write.address);
        if (address > 0x2000000 || write.bytes.size() > 0x2000000-address)
            throw runtime::MemoryFault("SIF RPC: output exceeds EE RAM");
        for (std::size_t offset=0; offset<write.bytes.size();) {
            const auto at = address + static_cast<std::uint32_t>(offset), base = at & ~15U;
            const auto size = std::min<std::size_t>(16-(at&15),write.bytes.size()-offset);
            auto [found,inserted] = quads.try_emplace(base);
            if (inserted && size != 16) {
                const auto original = ee.load_quad(base);
                for (unsigned i=0;i<16;++i) found->second[i] = static_cast<std::uint8_t>((i<8 ? original.lo : original.hi) >> ((i%8)*8));
            }
            std::copy_n(write.bytes.begin()+offset,size,found->second.begin()+(at&15)); offset += size;
        }
    }
    for (const auto& [address,bytes] : quads) {
        if (replies_.empty() || replies_.back().address + replies_.back().bytes.size() != address || replies_.back().bytes.size() == 0xffff0)
            replies_.push_back({address,{}});
        replies_.back().bytes.insert(replies_.back().bytes.end(),bytes.begin(),bytes.end());
    }
    fileio::Write command{ee_command_address_,Bytes(response.size()*4)};
    for (unsigned i=0;i<response.size();++i)
        for (unsigned byte=0;byte<4;++byte) command.bytes[i*4+byte] = static_cast<std::uint8_t>(response[i] >> (byte*8));
    replies_.push_back(std::move(command));
}
void Sif::reply_step() {
    if (transmit_.active || replies_.empty()) return;
    auto reply = std::move(replies_.front()); replies_.pop_front();
    constexpr std::uint32_t staging = 0x10000; // Private HLE allocation in IOP RAM.
    iop_install(staging,reply.bytes);
    iop_send(staging,reply.address,static_cast<std::uint32_t>(reply.bytes.size()/16));
    transmit_.end = replies_.empty();
}
}
