#pragma once
#include <nt/vu_native.hpp>
#include <algorithm>
#include <span>
#include <stdexcept>
#include <vector>
#include <memory>
#include <chrono>

namespace nt::vu {
using Entry = Result (*)(State*);
struct Block {
    std::uint32_t address;
    Entry entry;
    const Block* target=nullptr;
    const Block* fallthrough=nullptr;
};
enum class RunStatus { complete, unsupported_numeric, unmapped_block, budget_exhausted, unsupported_schedule, unsupported_memory, unsupported_context, unbound_program, graphics_pending };
struct RunResult {
    RunStatus status;
    std::uint64_t blocks;
};
// Optional conversion diagnostics. Entries group native block costs by unit
// and original entry index, including different uploaded artifacts at that index.
struct ExecutionProfile {
    struct Site { std::uint64_t calls=0,samples=0,nanoseconds=0; };
    std::vector<Site> sites;
    void merge(const ExecutionProfile& other) {
        if (other.sites.empty()) return;
        if (sites.empty()) sites.resize(4096);
        for (std::size_t i=0;i<other.sites.size();++i) {
            sites[i].calls+=other.sites[i].calls; sites[i].samples+=other.sites[i].samples;
            sites[i].nanoseconds+=other.sites[i].nanoseconds;
        }
    }
};

// Owns an immutable address index of trusted, ahead-of-time compiled functions.
// No instruction storage or decoding belongs in this execution interface.
class Program {
public:
    Program(unsigned unit, std::span<const Block> blocks) : unit_(unit),blocks_(std::make_shared<std::vector<Block>>(blocks.begin(),blocks.end())) {
        if (unit > 1) throw std::invalid_argument("Invalid VU unit");
        const auto capacity = unit == 0 ? 512U : 2048U;
        std::sort(blocks_->begin(), blocks_->end(), [](const Block& a, const Block& b) {
            return a.address < b.address;
        });
        for (std::size_t i = 0; i < blocks_->size(); ++i) {
            const auto& block = (*blocks_)[i];
            if (block.address >= capacity || !block.entry ||
                (i != 0 && (*blocks_)[i - 1].address == block.address))
                throw std::invalid_argument("Invalid or duplicate native VU block");
        }
        // Rebase linker references into our immutable owned graph. Program copies
        // share that graph, so callers need not retain their descriptor storage.
        for (auto& block:*blocks_) for (auto* link:{&block.target,&block.fallthrough}) {
            if (!*link) continue;
            const auto source=std::find_if(blocks.begin(),blocks.end(),[&](const Block& candidate) {
                return &candidate==*link;
            });
            if (source==blocks.end()) throw std::invalid_argument("Native VU successor leaves its artifact");
            *link=find(source->address);
        }
    }
    unsigned unit() const { return unit_; }
    bool contains(unsigned address) const { return find(address)!=nullptr; }
    bool within(unsigned begin,std::uint64_t end) const {
        return !blocks_->empty() && blocks_->front().address>=begin && blocks_->back().address<end;
    }

    // Completed blocks remain committed. A failing generated block is atomic.
    // next_pair identifies the next block when the budget expires or lookup fails.
    [[nodiscard]] RunResult run(State& state, std::uint64_t budget,ExecutionProfile* profile=nullptr) const {
        if (state.graphics_pending) return {RunStatus::graphics_pending,0};
        if (state.graphics_completes) return {RunStatus::complete,0};
        std::uint64_t count = 0;
        if (profile && profile->sites.empty()) profile->sites.resize(4096);
        auto found=find(state.next_pair);
        while (count < budget) {
            if (!found)
                return {RunStatus::unmapped_block, count};
            auto* site=profile ? &profile->sites[unit_*2048+found->address] : nullptr;
            const bool sample=site && (++site->calls%1024)==0;
            const auto began=sample ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            const auto result = found->entry(&state);
            if (sample) {
                site->nanoseconds+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-began).count();
                ++site->samples;
            }
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
            // Known control flow follows native linker references. Only genuinely
            // indirect or unbound exits need the existing address resolver.
            if (found->target && found->target->address==state.next_pair) found=found->target;
            else if (found->fallthrough && found->fallthrough->address==state.next_pair) found=found->fallthrough;
            else found=find(state.next_pair);
        }
        return {RunStatus::budget_exhausted, count};
    }
private:
    unsigned unit_;
    std::shared_ptr<std::vector<Block>> blocks_;
    const Block* find(unsigned address) const {
        const auto found=std::lower_bound(blocks_->begin(),blocks_->end(),address,
            [](const Block& block,unsigned value) { return block.address<value; });
        return found!=blocks_->end() && found->address==address ? &*found : nullptr;
    }
};
}
