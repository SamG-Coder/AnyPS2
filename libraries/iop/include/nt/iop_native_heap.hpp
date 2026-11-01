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
    NativeHeapServices(std::uint32_t memory_size,std::uint32_t heap_begin,std::uint32_t heap_end,std::uint32_t alignment=256)
        : memory_size_(memory_size), alignment_(alignment) {
        if (!heap_begin || heap_begin>heap_end || heap_end>memory_size || memory_size>0x200000 ||
            (alignment<8 || alignment>256 || (alignment&(alignment-1)) || ((memory_size|heap_begin|heap_end)&(alignment-1))))
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
    // Split a span out of one free block so mode 0 and mode 1 skip it. Mode 2
    // at this exact address and rounded size marks it allocated. Overlap with
    // an allocated, reserved, or held block returns false and leaves the map
    // unchanged. A claimed image returns to held storage on release: its AOT
    // addresses still identify immutable compiled code after module unload.
    bool hold(std::uint32_t address,std::uint32_t size) {
        if (!size || size>memory_size_ || (address&(alignment_-1)) || address>=memory_size_) return false;
        size=(size+alignment_-1)&~(alignment_-1);
        if (size>memory_size_-address) return false;
        auto it=std::prev(blocks_.upper_bound(address));
        if (it->second.state!=State::free) return false;
        const auto begin=it->first, end=begin+it->second.size;
        if (address<begin || address+size>end) return false;
        try {
            if (address>begin) blocks_.emplace(address,Block{size,State::held});
            if (address+size<end) blocks_.emplace(address+size,Block{end-address-size,State::free});
        } catch (...) {
            if (address>begin) blocks_.erase(address);
            if (address+size<end) blocks_.erase(address+size);
            throw;
        }
        it->second=address==begin ? Block{size,State::held} : Block{address-begin,State::free};
        return true;
    }
private:
    enum class State { free, allocated, reserved, held, claimed_image };
    struct Block { std::uint32_t size; State state; };
    std::uint32_t memory_size_,alignment_;
    std::map<std::uint32_t,Block> blocks_;

    std::uint32_t allocate(std::uint32_t mode,std::uint32_t size,std::uint32_t requested) {
        if (mode>2 || !size || size>memory_size_ || (mode==2 && (requested&(alignment_-1)))) return 0;
        size=(size+alignment_-1)&~(alignment_-1);
        if (mode==2) {
            const auto held=blocks_.find(requested);
            if (held!=blocks_.end() && held->second.state==State::held) {
                if (held->second.size!=size) return 0;
                held->second.state=State::claimed_image;
                return requested;
            }
        }
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
        if (it!=blocks_.end() && it->second.state==State::claimed_image) {
            it->second.state=State::held;
            return 0;
        }
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
