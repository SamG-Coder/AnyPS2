#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace nt::input {
// Standard PS2 button order, pressed=1. Device-specific mapping belongs to SDL.
struct PadSnapshot {
    bool connected=false;
    std::uint16_t buttons=0;
    std::array<std::uint8_t,4> axes{128,128,128,128}; // RX, RY, LX, LY
    bool operator==(const PadSnapshot&) const=default;
};
constexpr std::uint8_t pad_axis(std::int16_t value) {
    return static_cast<std::uint8_t>((static_cast<std::int32_t>(value)+32768)/256);
}
class Gamepads {
public:
    Gamepads();
    ~Gamepads();
    Gamepads(const Gamepads&)=delete;
    Gamepads& operator=(const Gamepads&)=delete;
    void update();
    PadSnapshot sample(unsigned port) const;
    std::string name(unsigned port) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
