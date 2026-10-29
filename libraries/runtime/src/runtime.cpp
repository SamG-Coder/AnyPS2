#include <nt/runtime.hpp>
#include <nt/ee_float.hpp>
#include <algorithm>

namespace nt::runtime {

FiniteFloatResult add_subtract_finite(std::uint32_t a,std::uint32_t b,bool subtract) {
    const auto result=numeric::add_subtract_finite(a,b,subtract);
    if (!result.supported) throw MemoryFault("Floating-point arithmetic: extended-range operands/results are not implemented");
    return {result.bits,result.underflow};
}

void divide_scalar(State& state, unsigned destination, unsigned left, unsigned right) {
    const auto a=state.fpr[left], b=state.fpr[right];
    const auto ea=(a>>23)&255, eb=(b>>23)&255;
    if (ea==255 || eb==255) throw MemoryFault("DIV.S: extended-range operands are not implemented");
    const auto sign=(a^b)&0x80000000U;
    auto result=sign, flags=state.fcr31 & ~0x30000U;
    if (!eb) {
        result |= 0x7f7fffff;
        flags |= ea ? 0x10020 : 0x20040;
    } else if (ea) {
        const auto numerator=(a&0x7fffff)|0x800000, denominator=(b&0x7fffff)|0x800000;
        const bool normalize=numerator<denominator;
        int exponent=static_cast<int>(ea)-static_cast<int>(eb)+127-(normalize ? 1 : 0);
        const auto scaled=std::uint64_t(numerator) << (normalize ? 24 : 23);
        auto quotient=scaled/denominator;
        const auto remainder=scaled%denominator;
        // Division uses nearest-even independently of the ordinary chop mode.
        if (remainder*2>denominator || (remainder*2==denominator && (quotient&1))) ++quotient;
        if (quotient==0x1000000) { quotient>>=1; ++exponent; }
        if (exponent>=255) throw MemoryFault("DIV.S: extended-range results are not implemented");
        if (exponent>0) result |= (static_cast<std::uint32_t>(exponent)<<23) |
                                 (static_cast<std::uint32_t>(quotient)&0x7fffff);
    }
    state.fpr[destination]=result; state.fcr31=flags;
}
void multiply_scalar(State& state, unsigned destination, unsigned left, unsigned right) {
    const auto result=numeric::multiply_finite(state.fpr[left],state.fpr[right]);
    if (!result.supported) throw MemoryFault("MUL.S: extended-range operands/results are not implemented");
    state.fpr[destination]=result.bits;
    state.fcr31=(state.fcr31&~0xc000U)|(result.underflow ? 0x4008U : 0U);
}
const char* name(Stop s) {
    switch (s) {
#define N(x) case Stop::x: return #x
    N(budget); N(syscall); N(breakpoint); N(unsupported); N(unmapped_code);
    N(code_changed); N(memory_fault); N(branch_in_delay);
    N(interrupt_pending);
#undef N
    }
    return "invalid";
}
RamBus::RamBus() : ram_(32 * 1024 * 1024, 0) {}
void RamBus::synchronize_cache(unsigned operation) {
    if (operation > 3) throw MemoryFault("Unsupported cache operation");
    // RAM is coherent. Generated instruction guards check every fetch against
    // current bytes, so a flush cannot expose stale translated instructions.
}
std::uint32_t default_ram_address(std::uint32_t address) {
    // BIOS-free default RAM mappings: uncached and uncached accelerated user
    // aliases above the reserved first MiB. Cache timing is not modeled.
    if ((address >= 0x20100000U && address < 0x22000000U) ||
        (address >= 0x30100000U && address < 0x32000000U)) address &= 0x0fffffffU;
    // KSEG0/KSEG1 are unmapped aliases. Other TLB mappings still fault.
    if (address >= 0x80000000U && address < 0xc0000000U) address &= 0x1fffffffU;
    return address;
}
std::span<std::uint8_t> RamBus::range(std::uint32_t address, unsigned size) {
    address = default_ram_address(address);
    if (address >= 0x70000000U && address < 0x70004000U) {
        const auto offset = address - 0x70000000U;
        if (!size || size > 8 || (size & (size-1)) || address % size || size > scratchpad_.size()-offset)
            throw MemoryFault("Address is not aligned scratchpad RAM");
        return std::span(scratchpad_).subspan(offset,size);
    }
    if (!size || size > 8 || (size & (size - 1)) || address % size ||
        address > ram_.size() || size > ram_.size() - address)
        throw MemoryFault("Address is not aligned diagnostic RAM");
    return std::span(ram_).subspan(address, size);
}
std::uint64_t RamBus::load(std::uint32_t address, unsigned size) {
    auto bytes = range(address, size);
    std::uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i) value |= std::uint64_t(bytes[i]) << (i * 8);
    return value;
}
void RamBus::store(std::uint32_t address, unsigned size, std::uint64_t value) {
    auto bytes = range(address, size);
    for (unsigned i = 0; i < size; ++i) bytes[i] = static_cast<std::uint8_t>(value >> (i * 8));
}
Register RamBus::load_quad(std::uint32_t address) {
    address &= ~15U; // EE LQ/SQ discard the low four address bits.
    range(address, 8); range(address + 8, 8);
    return {load(address, 8), load(address + 8, 8)};
}
void RamBus::store_quad(std::uint32_t address, Register value) {
    address &= ~15U;
    range(address, 8); range(address + 8, 8);
    store(address, 8, value.lo); store(address + 8, 8, value.hi);
}
void RamBus::install(std::uint32_t address, std::span<const std::uint8_t> data, std::uint32_t memory_size) {
    if (data.size() > memory_size || address > ram_.size() || memory_size > ram_.size() - address)
        throw MemoryFault("ELF segment is outside diagnostic RAM");
    std::fill_n(ram_.begin() + address, memory_size, std::uint8_t(0));
    std::copy(data.begin(), data.end(), ram_.begin() + address);
}
Register padduw(Register a, Register b) {
    const auto add = [](std::uint64_t x, std::uint64_t y) {
        const auto lo = std::min<std::uint64_t>(std::uint64_t(std::uint32_t(x)) + std::uint32_t(y), 0xffffffffULL);
        const auto hi = std::min<std::uint64_t>((x >> 32) + (y >> 32), 0xffffffffULL);
        return lo | (hi << 32);
    };
    return {add(a.lo, b.lo), add(a.hi, b.hi)};
}
}
