#include <nt/sif.hpp>

namespace nt::hardware {
std::uint32_t Sif::submit(std::span<const runtime::SifTransfer> transfers) {
    if (!job_.empty() || (send_.control & 0x100)) return 0;
    if (transfers.empty() || transfers.size() > 32 || !to_iop_.empty() || iop_packet_)
        throw runtime::MemoryFault("SIF DMA: invalid batch or receiver busy");
    std::vector<runtime::SifTransfer> validated;
    for (auto transfer : transfers) {
        const auto physical = runtime::default_ram_address(transfer.source);
        transfer.source = physical;
        if (transfer.bytes > 0xffff0 || transfer.source % 16 || transfer.destination % 4 || (transfer.attributes & ~0x44U))
            throw runtime::MemoryFault("SIF DMA: unsupported descriptor");
        transfer.bytes = (transfer.bytes + 15) & ~15U;
        const auto source_limit = physical >= 0x70000000U && physical < 0x70004000U ? 0x70004000U : 0x02000000U;
        if (physical > source_limit || transfer.bytes > source_limit - physical ||
            transfer.destination > iop_ram_.size() || transfer.bytes > iop_ram_.size() - transfer.destination)
            throw runtime::MemoryFault("SIF DMA: descriptor exceeds RAM");
        validated.push_back(transfer);
    }
    if (job_id_ == 0x7fffffff) throw runtime::MemoryFault("SIF DMA: exhausted job identifiers");
    job_ = std::move(validated); job_index_ = 0; job_offset_ = 0; job_tagged_ = false;
    job_completion_ = received_packets_ + job_.size();
    return ++job_id_;
}
std::int32_t Sif::status(std::uint32_t id) const {
    if (!id || id > job_id_) return -1;
    return id == job_id_ && !job_.empty() ? 1 : -1;
}
void Sif::job_step(runtime::RamBus& ee) {
    if (job_.empty()) return;
    if (job_index_ == job_.size()) {
        if (received_packets_ >= job_completion_) { job_.clear(); completion_ |= 1U << 6; }
        return;
    }
    if (to_iop_.size() > 124) return;
    const auto& transfer = job_[job_index_];
    if (!job_tagged_) {
        to_iop_.push_back(transfer.destination | 0x80000000U);
        to_iop_.push_back(transfer.bytes / 4); to_iop_.push_back(0); to_iop_.push_back(0);
        job_tagged_ = true;
    } else if (job_offset_ < transfer.bytes) {
        const auto data = ee.load_quad(transfer.source + job_offset_);
        for (auto half : {data.lo,data.hi}) {
            to_iop_.push_back(static_cast<std::uint32_t>(half));
            to_iop_.push_back(static_cast<std::uint32_t>(half >> 32));
        }
        job_offset_ += 16;
    } else { ++job_index_; job_offset_ = 0; job_tagged_ = false; }
}
}
