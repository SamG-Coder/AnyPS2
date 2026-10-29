#include <nt/iop_heaplib.hpp>
#include <nt/iop_memory.hpp>
#include <algorithm>
#include <stdexcept>

namespace nt::iop {
namespace {
CallResult value(std::uint32_t result) { return {CallState::returned,result}; }
}
Heaplib::Heaplib(std::uint32_t base,Allocate allocate,Release release)
    : library_("heaplib",0x101,18,base), allocate_(std::move(allocate)), release_(std::move(release)) {
    if (!allocate_ || !release_) throw std::runtime_error("IOP heaplib: missing memory service");
}
Heaplib::Block Heaplib::make_block(HeapRegion region,std::uint32_t usable_size,unsigned overhead,std::size_t ram_size) const {
    if (!region.base || region.base%8 || region.base>=ram_size || region.size>ram_size-region.base ||
        usable_size>region.size || usable_size<overhead+16)
        throw std::runtime_error("IOP heaplib: invalid backing allocation");
    const auto overlaps=[&](std::uint32_t base,std::size_t size) {
        return region.base<std::uint64_t(base)+size && base<std::uint64_t(region.base)+region.size;
    };
    if (overlaps(module().base,module().memory.size())) throw std::runtime_error("IOP heaplib: backing overlaps provider");
    for (const auto& [handle,heap] : heaps_)
        for (const auto& block : heap.blocks)
            if (overlaps(block.backing.base,block.backing.size)) throw std::runtime_error("IOP heaplib: overlapping heaps");
    Block block{region,{}, {}};
    block.free.emplace(region.base+overhead,(usable_size-overhead-8)&~7U);
    return block;
}
std::uint32_t Heaplib::allocate_block(Block& block,std::uint32_t size) {
    for (auto it=block.free.begin();it!=block.free.end();++it) {
        if (it->second<size) continue;
        const auto at=it->first, remaining=it->second-size;
        block.free.erase(it);
        if (remaining) block.free.emplace(at+size,remaining);
        block.used.emplace(at+8,size);
        return at+8;
    }
    return 0;
}
void Heaplib::free_block(Block& block,std::map<std::uint32_t,std::uint32_t>::iterator allocation) {
    auto at=allocation->first-8, size=allocation->second;
    block.used.erase(allocation);
    auto next=block.free.lower_bound(at);
    if (next!=block.free.begin()) {
        const auto previous=std::prev(next);
        if (previous->first+previous->second==at) {
            at=previous->first; size+=previous->second; block.free.erase(previous);
        }
    }
    if (next!=block.free.end() && at+size==next->first) { size+=next->second; block.free.erase(next); }
    block.free.emplace(at,size);
}
CallResult Heaplib::invoke(std::uint32_t pc,const std::array<std::uint32_t,4>& a,std::span<std::uint8_t> ram,bool interrupt_context) {
    const auto ordinal=library_.ordinal(pc,ram.size());
    if (!ordinal || *ordinal<4 || *ordinal>8 || interrupt_context) return {};
    if (*ordinal==4) {
        if (a[1]&~3U) return {};
        if (a[0]<56 || a[0]>ram.size()-3) return value(0);
        const auto size=(a[0]+3)&~3U;
        const auto backing=allocate_(size,(a[1]>>1)&1);
        if (!backing.base) return value(0);
        auto block=make_block(backing,size,32,ram.size());
        Heap heap{size,a[1],{}}; heap.blocks.push_back(std::move(block));
        heaps_.emplace(backing.base,std::move(heap));
        return value(backing.base);
    }
    const auto handle=ram_offset(a[0],4,ram.size());
    const auto found=handle ? heaps_.find(*handle) : heaps_.end();
    if (found==heaps_.end()) return value(*ordinal==6 || *ordinal==5 ? 0 : std::uint32_t(-4));
    auto& heap=found->second;
    if (*ordinal==5) {
        while (!heap.blocks.empty()) {
            if (!release_(heap.blocks.back().backing.base)) return {};
            heap.blocks.pop_back();
        }
        heaps_.erase(found);
        return value(0);
    }
    if (*ordinal==6) {
        if (a[1]>ram.size()-48) return value(0);
        const auto footprint=((std::max(a[1],8U)+7)&~7U)+8;
        for (auto& block : heap.blocks) if (const auto result=allocate_block(block,footprint)) return value(result);
        if (!(heap.flags&1)) return value(0);
        const auto size=std::max(heap.block_size,footprint+32);
        const auto backing=allocate_(size,(heap.flags>>1)&1);
        if (!backing.base) return value(0);
        auto block=make_block(backing,backing.size,24,ram.size());
        const auto result=allocate_block(block,footprint);
        heap.blocks.push_back(std::move(block));
        return value(result);
    }
    if (*ordinal==7) {
        const auto pointer=ram_offset(a[1],8,ram.size());
        if (!pointer) return value(std::uint32_t(-1));
        for (auto it=heap.blocks.begin();it!=heap.blocks.end();++it) {
            const auto allocation=it->used.find(*pointer);
            if (allocation==it->used.end()) continue;
            if (it!=heap.blocks.begin() && it->used.size()==1) {
                if (!release_(it->backing.base)) return {};
                heap.blocks.erase(it);
            } else free_block(*it,allocation);
            return value(0);
        }
        return value(std::uint32_t(-1));
    }
    std::uint32_t total=0;
    for (const auto& block : heap.blocks) for (const auto& [at,size] : block.free) total+=size;
    return value(total);
}
}
