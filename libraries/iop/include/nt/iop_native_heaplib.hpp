#pragma once
#include <nt/iop_native_heap.hpp>
#include <vector>

namespace nt::iop {
// Opaque heap handles and host-owned subarenas backed by the shared allocator.
class NativeHeapLibraryServices final : public NativeServices {
public:
    explicit NativeHeapLibraryServices(NativeHeapServices& system) : system_(system) {}
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t> ram) override {
        if (request.kind!=NativeServiceKind::heaplib || request.selector<4 || request.selector>8) return {};
        const auto memory=system_.invoke({6,{},NativeServiceKind::sysmem},ram);
        if (memory.status!=NativeServiceStatus::returned) return memory;
        const auto& a=request.arguments;
        if (request.selector==4) {
            // Smaller arenas cannot contain a usable chunk; avoid reference overflow paths.
            if (a[0]<60 || a[0]>memory.value || (a[1]&~3U)) return {NativeServiceStatus::unavailable};
            Heap heap{(a[0]+3)&~3U,a[1],{}};
            const auto address=reserve(heap,heap.size,true,memory.value,ram);
            if (!address) return returned(0);
            try { heaps_.emplace(address,std::move(heap)); }
            catch (...) { release(address,ram); throw; }
            return returned(address);
        }
        const auto handle=ram_offset(a[0],1,memory.value);
        const auto found=handle ? heaps_.find(*handle) : heaps_.end();
        if (found==heaps_.end()) return returned(request.selector==6 || request.selector==5 ? 0U : std::uint32_t(-4));
        auto& heap=found->second;
        if (request.selector==5) {
            for (const auto& chunk : heap.chunks) release(chunk.base,ram);
            heaps_.erase(found); return returned(0);
        }
        if (request.selector==8) {
            std::uint32_t total=0;
            for (auto& chunk : heap.chunks) total+=chunk.arena.invoke({8,{},NativeServiceKind::sysmem},ram).value;
            return returned(total);
        }
        if (request.selector==7) {
            const auto pointer=ram_offset(a[1],8,memory.value);
            if (!pointer || *pointer<8) return returned(std::uint32_t(-1));
            for (auto it=heap.chunks.begin();it!=heap.chunks.end();++it) {
                const auto freed=it->arena.invoke({5,{*pointer-8},NativeServiceKind::sysmem},ram);
                if (freed.value) continue;
                --it->allocations;
                if (it!=heap.chunks.begin() && !it->allocations) { release(it->base,ram); heap.chunks.erase(it); }
                return returned(0);
            }
            return returned(std::uint32_t(-1));
        }
        if (a[1]>memory.value || a[1]>0x200000-40) return returned(0);
        const auto bytes=((std::max(a[1],8U)+7)&~7U)+8;
        for (auto& chunk : heap.chunks) if (const auto address=allocate(chunk,bytes,ram)) return returned(address);
        if (!(heap.flags&1)) return returned(0);
        if (!reserve(heap,std::max(heap.size,a[1]+40),false,memory.value,ram)) return returned(0);
        return returned(allocate(heap.chunks.back(),bytes,ram));
    }
private:
    struct Chunk { std::uint32_t base,allocations; NativeHeapServices arena; };
    struct Heap { std::uint32_t size,flags; std::vector<Chunk> chunks; };
    NativeHeapServices& system_;
    std::map<std::uint32_t,Heap> heaps_;
    static NativeServiceReply returned(std::uint32_t value) { return {NativeServiceStatus::returned,value}; }
    void release(std::uint32_t address,std::span<std::uint8_t> ram) {
        system_.invoke({5,{address},NativeServiceKind::sysmem},ram);
    }
    std::uint32_t reserve(Heap& heap,std::uint32_t bytes,bool initial,std::uint32_t memory,std::span<std::uint8_t> ram) {
        const auto address=system_.invoke({4,{(heap.flags>>1)&1,bytes,0},NativeServiceKind::sysmem},ram).value;
        if (!address) return 0;
        try {
            const auto extent=initial ? bytes : system_.invoke({10,{address},NativeServiceKind::sysmem},ram).value;
            const auto begin=address+(initial ? 32U : 24U),end=address+((extent-8)&~7U);
            heap.chunks.push_back({address,0,NativeHeapServices(memory,begin,end,8)});
        } catch (...) { release(address,ram); throw; }
        return address;
    }
    static std::uint32_t allocate(Chunk& chunk,std::uint32_t bytes,std::span<std::uint8_t> ram) {
        const auto address=chunk.arena.invoke({4,{0,bytes,0},NativeServiceKind::sysmem},ram).value;
        if (!address) return 0;
        ++chunk.allocations; return address+8;
    }
};
}
