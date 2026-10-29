#pragma once
#include <cstdint>
#include <optional>
#include <nt/iop_dma.hpp>
#include <nt/iop_timers.hpp>
#include <nt/iop_cdvd.hpp>
#include <nt/iop_resources.hpp>

namespace nt::iop {
class Sbus {
public:
    virtual ~Sbus()=default;
    virtual std::optional<std::uint32_t> read_sbus(std::uint32_t offset) const=0;
    virtual bool write_sbus(std::uint32_t offset,std::uint32_t value)=0;
};
class InterruptController {
public:
    void raise(std::uint32_t causes) { status_|=causes; }
    bool pending() const { return (control_&1) && (status_&mask_); }
    std::uint32_t pending_sources() const { return (control_&1) ? status_&mask_ : 0; }
    std::optional<std::uint32_t> read(std::uint32_t address,unsigned bytes);
    bool write(std::uint32_t address,unsigned bytes,std::uint32_t value);
private:
    std::uint32_t status_=0, mask_=0, control_=0;
};
class Devices {
public:
    virtual ~Devices()=default;
    // Physical device addresses, or unmapped kernel control addresses.
    // A rejected operation must not mutate devices.
    virtual std::optional<std::uint32_t> read(std::uint32_t address,unsigned bytes)=0;
    virtual bool write(std::uint32_t address,unsigned bytes,std::uint32_t value)=0;
    virtual bool interrupt_pending() const { return false; }
    // Diagnostic execution cadence, not a cycle-accurate IOP clock.
    virtual void instruction_retired() {}
};
class BootstrapDevices final : public Devices {
public:
    explicit BootstrapDevices(Sbus* sbus=nullptr) : sbus_(sbus) {}
    std::optional<std::uint32_t> read(std::uint32_t address,unsigned bytes) override;
    bool write(std::uint32_t address,unsigned bytes,std::uint32_t value) override;
    bool interrupt_pending() const override { return interrupts_.pending(); }
    std::uint32_t pending_sources() const { return interrupts_.pending_sources(); }
    std::uint32_t pending_dma_channels() const { return dma_.pending_channels(); }
    bool forced_dma_interrupt() const { return dma_.forced_interrupt(); }
    void instruction_retired() override { interrupts_.raise(timers_.tick()); }
    InterruptController& interrupts() { return interrupts_; }
    void enable_sif1_receive() { dma_.enable_sif1_receive(); }
    void enable_sif0_send() { dma_.enable_sif0_send(); }
    bool send_sif0(std::deque<std::uint32_t>& fifo,std::span<const std::uint8_t> ram) {
        const auto completed=dma_.send_sif0(fifo,ram);
        if (completed && dma_.interrupt_pending()) interrupts_.raise(8);
        return completed;
    }
    bool receive_sif1(std::deque<std::uint32_t>& fifo,std::span<std::uint8_t> ram) {
        const auto completed=dma_.receive_sif1(fifo,ram);
        if (completed && dma_.interrupt_pending()) interrupts_.raise(8);
        return completed;
    }
    bool signal_dma_completion(unsigned channel) {
        if (!dma_.signal_completion(channel)) return false;
        if (dma_.interrupt_pending()) interrupts_.raise(8);
        return true;
    }
private:
    Sbus* sbus_;
    InterruptController interrupts_;
    DmaController dma_;
    Timers timers_;
    Cdvd cdvd_;
    Resources resources_;
    std::uint32_t configuration_=0;
    std::uint32_t dma_control_=0;
    std::uint32_t cache_control_=0;
};
}
