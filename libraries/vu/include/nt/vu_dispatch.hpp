#pragma once
#include <nt/vu_native.hpp>
#include <algorithm>
#include <span>
#include <stdexcept>
#include <vector>

namespace nt::vu {
using Entry = Result (*)(State*);
struct Block {
    std::uint32_t address;
    Entry entry;
};
enum class RunStatus { complete, unsupported_numeric, unmapped_block, budget_exhausted, unsupported_schedule, unsupported_memory, unsupported_context, unbound_program, graphics_pending };
struct RunResult {
    RunStatus status;
    std::uint64_t blocks;
};

// Owns an immutable address index of trusted, ahead-of-time compiled functions.
// No instruction storage or decoding belongs in this execution interface.
class Program {
public:
    Program(unsigned unit, std::span<const Block> blocks) : unit_(unit),blocks_(blocks.begin(), blocks.end()) {
        if (unit > 1) throw std::invalid_argument("Invalid VU unit");
        const auto capacity = unit == 0 ? 512U : 2048U;
        std::sort(blocks_.begin(), blocks_.end(), [](const Block& a, const Block& b) {
            return a.address < b.address;
        });
        for (std::size_t i = 0; i < blocks_.size(); ++i) {
            const auto& block = blocks_[i];
            if (block.address >= capacity || !block.entry ||
                (i != 0 && blocks_[i - 1].address == block.address))
                throw std::invalid_argument("Invalid or duplicate native VU block");
        }
    }
    unsigned unit() const { return unit_; }
    bool within(unsigned begin,std::uint64_t end) const {
        return !blocks_.empty() && blocks_.front().address>=begin && blocks_.back().address<end;
    }

    // Completed blocks remain committed. A failing generated block is atomic.
    // next_pair identifies the next block when the budget expires or lookup fails.
    [[nodiscard]] RunResult run(State& state, std::uint64_t budget) const {
        if (state.graphics_pending) return {RunStatus::graphics_pending,0};
        if (state.graphics_completes) return {RunStatus::complete,0};
        std::uint64_t count = 0;
        while (count < budget) {
            const auto found = std::lower_bound(blocks_.begin(), blocks_.end(), state.next_pair,
                [](const Block& block, std::uint32_t address) { return block.address < address; });
            if (found == blocks_.end() || found->address != state.next_pair)
                return {RunStatus::unmapped_block, count};
            const auto result = found->entry(&state);
            ++count;
            switch (result) {
            case Result::complete: return {RunStatus::complete, count};
            case Result::unsupported_numeric: return {RunStatus::unsupported_numeric, count};
            case Result::unsupported_schedule: return {RunStatus::unsupported_schedule, count};
            case Result::unsupported_memory: return {RunStatus::unsupported_memory, count};
            case Result::unsupported_context: return {RunStatus::unsupported_context, count};
            case Result::graphics_request: return {RunStatus::graphics_pending, count};
            case Result::transfer: break;
            }
        }
        return {RunStatus::budget_exhausted, count};
    }
private:
    unsigned unit_;
    std::vector<Block> blocks_;
};
}
