#pragma once
#include <nt/gamepad.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nt::input {
// Timed button presses for reproducible unattended runs (measurement and
// regression), replacing the host controller on port 0. One event per line:
//   <start-ms> <hold-ms> <button>[+<button>...]
// Times count from the start of execution. Button names: select l3 r3 start up
// right down left l2 r2 l1 r1 triangle circle cross square. '#' starts a comment.
struct PadPress { std::uint64_t start_ms=0,hold_ms=0; std::uint16_t buttons=0; };
class PadScript {
public:
    static constexpr std::size_t max_events=4096;
    // Returns an empty error on success; otherwise names the line and problem.
    std::string parse(std::string_view text);
    PadSnapshot sample(std::uint64_t elapsed_ms) const;
    // The time after which no scripted press remains.
    std::uint64_t end_ms() const;
    const std::vector<PadPress>& presses() const { return presses_; }
private:
    std::vector<PadPress> presses_;
};
}
