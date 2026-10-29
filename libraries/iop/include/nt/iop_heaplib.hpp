#pragma once
#include <nt/iop_calls.hpp>
#include <nt/iop_native_library.hpp>
#include <functional>
#include <map>

namespace nt::iop {
struct HeapRegion { std::uint32_t base=0, size=0; };
class Heaplib final : public HostCalls {
public:
    using Allocate=std::function<HeapRegion(std::uint32_t size,unsigned policy)>;
    using Release=std::function<bool(std::uint32_t base)>;
    Heaplib(std::uint32_t base,Allocate allocate,Release release);
    static constexpr unsigned allocation_size() { return NativeLibrary::allocation_size(18); }
    void install(std::span<std::uint8_t> ram,Libraries& libraries) { library_.install(ram,libraries); }
    const elf::IopModule& module() const { return library_.module(); }
    bool owns(std::uint32_t pc,std::size_t size) const override { return library_.ordinal(pc,size).has_value(); }
    std::uint32_t registration_flag() const { return library_.registration_flag(); }
    CallResult invoke(std::uint32_t pc,const std::array<std::uint32_t,4>& arguments,
                      std::span<std::uint8_t> ram,bool interrupt_context) override;
private:
    struct Block {
        HeapRegion backing;
        std::map<std::uint32_t,std::uint32_t> free, used;
    };
    struct Heap { std::uint32_t block_size, flags; std::vector<Block> blocks; };
    NativeLibrary library_;
    Allocate allocate_;
    Release release_;
    std::map<std::uint32_t,Heap> heaps_;
    Block make_block(HeapRegion region,std::uint32_t usable_size,unsigned overhead,std::size_t ram_size) const;
    static std::uint32_t allocate_block(Block& block,std::uint32_t size);
    static void free_block(Block& block,std::map<std::uint32_t,std::uint32_t>::iterator allocation);
};
}
