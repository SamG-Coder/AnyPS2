#pragma once
#include <nt/iop_native_services.hpp>
#include <algorithm>
#include <map>
#include <iterator>
#include <stdexcept>

namespace nt::iop {
// Host-owned allocation metadata. Guest addresses remain bounded offsets into
// the converted program's data arena; no guest allocator code or tables execute.
class NativeHeapServices final : public NativeServices {
public:
    NativeHeapServices(std::uint32_t memory_size,std::uint32_t heap_begin,std::uint32_t heap_end)
        : memory_size_(memory_size) {
        if (!heap_begin || heap_begin>heap_end || heap_end>memory_size || memory_size>0x200000 ||
            ((memory_size|heap_begin|heap_end)&255))
            throw std::invalid_argument("Native heap requires aligned, bounded arena and reserved low memory");
        blocks_.emplace(0,Block{heap_begin,State::reserved});
        if (heap_begin<heap_end) blocks_.emplace(heap_begin,Block{heap_end-heap_begin,State::free});
        if (heap_end<memory_size) blocks_.emplace(heap_end,Block{memory_size-heap_end,State::reserved});
    }
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t> ram) override {
        if (request.kind!=NativeServiceKind::sysmem || request.selector<4 || request.selector>10) return {};
        if (ram.size()<memory_size_) return {NativeServiceStatus::address_fault,0,memory_size_};
        const auto& args=request.arguments;
        std::uint32_t value=0;
        switch (request.selector) {
        case 4: value=allocate(args[0],args[1],args[2]); break;
        case 5: value=release(args[0]); break;
        case 6: value=memory_size_; break;
        case 7: case 8:
            for (const auto& [address,block] : blocks_) if (block.state==State::free)
                value=request.selector==7 ? std::max(value,block.size) : value+block.size;
            break;
        case 9: case 10: {
            const auto address=args[0];
            if (address>=memory_size_) { value=0xffffffff; break; }
            const auto block=std::prev(blocks_.upper_bound(address));
            value=request.selector==9 ? block->first : block->second.size;
            if (block->second.state==State::free) value|=0x80000000;
            break;
        }
        }
        return {NativeServiceStatus::returned,value};
    }
private:
    enum class State { free, allocated, reserved };
    struct Block { std::uint32_t size; State state; };
    std::uint32_t memory_size_;
    std::map<std::uint32_t,Block> blocks_;

    std::uint32_t allocate(std::uint32_t mode,std::uint32_t size,std::uint32_t requested) {
        if (mode>2 || !size || size>memory_size_ || (mode==2 && (requested&255))) return 0;
        size=(size+255)&~255U;
        auto chosen=blocks_.end();
        std::uint32_t start=0;
        for (auto it=blocks_.begin();it!=blocks_.end();++it) {
            if (it->second.state!=State::free || it->second.size<size) continue;
            const auto begin=it->first, end=begin+it->second.size;
            if (mode==2 && (requested<begin || requested>end-size)) continue;
            chosen=it; start=mode==2 ? requested : mode==1 ? end-size : begin;
            if (mode!=1) break;
        }
        if (chosen==blocks_.end()) return 0;
        const auto begin=chosen->first, end=begin+chosen->second.size;
        // Prepare splits before changing the original free extent. Roll back on
        // host allocation failure so retrying cannot expose overlapping blocks.
        try {
            if (start>begin) blocks_.emplace(start,Block{size,State::allocated});
            if (start+size<end) blocks_.emplace(start+size,Block{end-start-size,State::free});
        } catch (...) {
            if (start>begin) blocks_.erase(start);
            if (start+size<end) blocks_.erase(start+size);
            throw;
        }
        chosen->second=start==begin ? Block{size,State::allocated} : Block{start-begin,State::free};
        return start;
    }
    std::uint32_t release(std::uint32_t address) {
        auto it=blocks_.find(address);
        if (it==blocks_.end() || it->second.state!=State::allocated) return 0xffffffff;
        it->second.state=State::free;
        const auto next=std::next(it);
        if (next!=blocks_.end() && next->second.state==State::free) {
            it->second.size+=next->second.size; blocks_.erase(next);
        }
        if (it!=blocks_.begin()) {
            const auto previous=std::prev(it);
            if (previous->second.state==State::free) {
                previous->second.size+=it->second.size; blocks_.erase(it);
            }
        }
        return 0;
    }
};
}
