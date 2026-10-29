#pragma once
#include <nt/runtime.hpp>
#include <nt/sif.hpp>
#include <nt/gs.hpp>
#include <nt/gif.hpp>
#include <nt/graphics_dma.hpp>
#include <nt/vif.hpp>
#include <nt/vu_control.hpp>

namespace nt::hardware {
// Incremental hardware boundary. Unknown registers and unsupported active modes
// fault; they must never read as arbitrary zeroes.
class SystemBus final : public runtime::Bus {
public:
    std::uint64_t load(std::uint32_t address, unsigned size) override;
    void store(std::uint32_t address, unsigned size, std::uint64_t value) override;
    runtime::Register load_quad(std::uint32_t address) override;
    void store_quad(std::uint32_t address, runtime::Register value) override;
    void synchronize_cache(unsigned operation) override { ram_.synchronize_cache(operation); }
    void advance_cycles(std::uint64_t cycles) override;
    runtime::Register read_cop2_vector(unsigned reg) override { return vu_.vector(reg); }
    void write_cop2_vector(unsigned reg, runtime::Register value) override { vu_.set_vector(reg,value); }
    void add_subtract_cop2_vectors(unsigned destination, unsigned left, unsigned right, unsigned mask, bool subtract, unsigned broadcast) override {
        vu_.add_subtract(destination,left,right,mask,subtract,broadcast);
    }
    std::uint32_t read_cop2_control(unsigned reg) override { return vu_.read(reg); }
    void write_cop2_control(unsigned reg, std::uint32_t value) override { vu_.write(reg,value); }
    void set_gs_crt(std::uint32_t interlace, std::uint32_t mode, std::uint32_t frame) override {
        gs_.set_crt(interlace,mode,frame);
    }
    bool interrupt_pending() const override { return interrupt_lines() != 0; }
    std::uint32_t interrupt_lines() const override {
        return ((interrupt_status_ & interrupt_mask_) ? 0x400U : 0U) |
               ((dma_status_ & (dma_status_ >> 16) & 0x3ff) ? 0x800U : 0U);
    }
    Sif& sif() { return sif_; }
    void initialize_sif() override { sif_.initialize_commands(); }
    std::uint32_t submit_sif(std::span<const runtime::SifTransfer> transfers) override { return sif_.submit(transfers); }
    std::int32_t sif_status(std::uint32_t id) const override { return sif_.status(id); }
    std::span<const std::uint8_t> snapshot() const { return ram_.snapshot(); }
    void install(std::uint32_t address, std::span<const std::uint8_t> data, std::uint32_t size) { ram_.install(address,data,size); }
    std::uint32_t last_address = 0;
    unsigned last_size = 0;
    bool last_write = false;
private:
    struct Timer { std::uint32_t count = 0, mode = 0, target = 0xffff, hold = 0, phase = 0; };
    std::array<Timer,4> timers_{};
    std::uint32_t interrupt_status_ = 0, interrupt_mask_ = 0;
    std::uint32_t dma_status_ = 0;
    std::uint32_t dma_control_ = 0, dma_enable_ = 0x1201;
    std::uint32_t dma_priority_ = 0;
    Sif sif_;
    Gs gs_;
    Gif gif_;
    GraphicsDma graphics_dma_;
    Vif vif_;
    VuControl vu_;
    std::uint64_t cycles_ = 0;
    runtime::RamBus ram_;
    static std::uint32_t physical(std::uint32_t address);
};
}
