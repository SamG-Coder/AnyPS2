#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <deque>
#include <span>

namespace nt::iop {
class DmaController {
public:
    static std::optional<std::uint32_t> channel_register(unsigned channel,unsigned offset) {
        if (channel>=13 || offset%4 || offset>12 || (offset==12 && channel!=4 && channel!=9)) return {};
        return (channel<7 ? 0x1f801080+channel*16 : 0x1f801500+(channel-7)*16)+offset;
    }
    std::optional<std::uint32_t> read(std::uint32_t address,unsigned bytes) const;
    bool write(std::uint32_t address,unsigned bytes,std::uint32_t value);
    bool interrupt_pending() const;
    std::uint32_t pending_channels() const;
    bool forced_interrupt() const { return ((interrupts_[0]|interrupts_[1])&0x8000)!=0; }
    bool signal_completion(unsigned channel);
    void enable_sif1_receive() { sif1_connected_=true; }
    void enable_sif0_send() { sif0_connected_=true; }
    bool send_sif0(std::deque<std::uint32_t>& fifo,std::span<const std::uint8_t> ram);
    // One bounded FIFO step; true means a destination packet finished.
    bool receive_sif1(std::deque<std::uint32_t>& fifo,std::span<std::uint8_t> ram);
private:
    std::array<std::array<std::uint32_t,4>,13> channels_{};
    std::uint32_t priority_=0, priority2_=0;
    std::array<std::uint32_t,2> interrupts_{};
    bool sif1_connected_=false, sif1_packet_=false, sif1_end_=false;
    std::uint32_t sif1_remaining_=0;
    bool sif0_connected_=false, sif0_packet_=false, sif0_end_=false;
    std::uint32_t sif0_remaining_=0;
    std::array<std::uint32_t,4> sif0_previous_{};
};
}
