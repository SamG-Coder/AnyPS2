#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <chrono>

namespace nt::iop {
// CD/DVD controller initialization with a public register compatibility profile.
// No firmware, NVM or media contents are embedded in the device model.
class Cdvd {
public:
    using Clock=std::chrono::system_clock::time_point (*)();
    explicit Cdvd(Clock clock=std::chrono::system_clock::now) : clock_(clock) {}
    std::optional<std::uint32_t> read(std::uint32_t address,unsigned bytes);
    bool write(std::uint32_t address,unsigned bytes,std::uint32_t value);
    bool interrupt_pending() const { return interrupt_!=0; }
private:
    // Public PCSX2 fallback profile, in command-response byte order. This selects
    // a register ABI; it is not a claim that all commands are implemented.
    static constexpr std::array<std::uint8_t,4> mechanical_version_{3,6,2,0};
    Clock clock_;
    std::array<std::uint8_t,16> normal_parameters_{}, sub_parameters_{}, result_{};
    unsigned normal_size_=0,sub_size_=0,result_size_=0,result_position_=0;
    std::uint8_t normal_command_=0,sub_command_=0,ready_=0x40,error_=0;
    std::uint8_t interrupt_=0,status_=0,sticky_=0,how_to_=0;
    bool subcommand(std::uint8_t command);
};
}
