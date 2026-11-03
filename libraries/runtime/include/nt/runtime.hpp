#pragma once
#include <nt/ee_state.hpp>
#include <nt/ee_address.hpp>
#include <nt/ee_float.hpp>
#include <array>
#include <nt/word_merge.hpp>
#include <bit>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace nt::runtime {
inline Register masked_vector(Register original, Register value, unsigned mask) {
    const auto low = ((mask & 8) ? 0xffffffffULL : 0ULL) | ((mask & 4) ? 0xffffffff00000000ULL : 0ULL);
    const auto high = ((mask & 2) ? 0xffffffffULL : 0ULL) | ((mask & 1) ? 0xffffffff00000000ULL : 0ULL);
    return {(original.lo & ~low) | (value.lo & low),(original.hi & ~high) | (value.hi & high)};
}
inline Register rotate_vector(Register value) {
    return {(value.lo >> 32) | (value.hi << 32),(value.hi >> 32) | (value.lo << 32)};
}
enum class Stop { budget, syscall, breakpoint, unsupported, unmapped_code, code_changed, memory_fault, branch_in_delay, interrupt_pending };
inline void compare_scalar(State& state, unsigned left, unsigned right, unsigned condition) {
    const auto ordered = [](std::uint32_t bits) {
        const auto exponent = bits & 0x7f800000U;
        if (!exponent) bits = 0; // Signed zero and denormals compare equally.
        else if (exponent == 0x7f800000U) bits = (bits & 0x80000000U) | 0x7f7fffff;
        return (bits >> 31) ? ~bits : bits | 0x80000000U;
    };
    const auto a = ordered(state.fpr[left]), b = ordered(state.fpr[right]);
    const bool result = ((condition & 2) && a == b) || ((condition & 4) && a < b);
    state.fcr31 = (state.fcr31 & ~0x800000U) | (result ? 0x800000U : 0);
}
using numeric::integer_to_scalar;
using numeric::scalar_to_integer;
using numeric::min_max_scalar;
struct Result { Stop reason; std::uint32_t pc, word; std::string detail{}; };
const char* name(Stop stop);
class MemoryFault : public std::runtime_error { public: using std::runtime_error::runtime_error; };
void multiply_scalar(State& state, unsigned destination, unsigned left, unsigned right);
struct FiniteFloatResult { std::uint32_t bits; bool underflow = false; };
FiniteFloatResult add_subtract_finite(std::uint32_t left, std::uint32_t right, bool subtract);
inline void add_subtract_scalar(State& state, unsigned destination, unsigned left, unsigned right, bool subtract) {
    const auto result=add_subtract_finite(state.fpr[left],state.fpr[right],subtract);
    state.fpr[destination]=result.bits;
    state.fcr31=(state.fcr31 & ~0xc000U) | (result.underflow ? 0x4008U : 0);
}
void divide_scalar(State& state, unsigned destination, unsigned left, unsigned right);
// Translate only the BIOS-free default RAM aliases; callers enforce RAM bounds.
struct SifTransfer { std::uint32_t source, destination, bytes, attributes; };
// Diagnostic execution boundary. Production output links native service owners.
class Bus {
public:
    virtual ~Bus() = default;
    std::uint32_t read_performance(unsigned selector) const {
        return performance_[(selector & 1) ? 1 + ((selector >> 1) & 1) : 0];
    }
    void write_performance(unsigned selector, std::uint32_t value) {
        if (!(selector & 1)) {
            if (selector & 62) return; // MTPS writes only selector zero.
            if (value & 0x80000000U) {
                for (unsigned shift : {5U,15U}) {
                    const auto event = (value >> shift) & 31;
                    if (event != 0 && event != 1 && event != 6 && event < 16)
                        throw MemoryFault("CP0: performance event is not implemented");
                }
            }
        }
        performance_[(selector & 1) ? 1 + ((selector >> 1) & 1) : 0] = value;
    }
    void advance_performance(std::uint32_t status, std::uint64_t cycles) {
        const auto control = performance_[0];
        if (!(control & 0x80000000U) || (status & 4)) return;
        auto next = performance_;
        for (unsigned counter=0;counter<2;++counter) {
            const auto shift = counter*10;
            const auto permission = (1U << (((status>>3)&3)+2+shift)) |
                                    ((status & 2) ? 1U << (1+shift) : 0);
            // Only diagnostic cycles produce events. Cache misses remain zero
            // in the runtime's coherent-memory model; disabled modes stay idle.
            if ((control & permission) && ((control >> (5+shift)) & 31) == 1) {
                if (cycles >= 0x80000000ULL || next[counter+1] >= 0x80000000ULL-cycles)
                    throw MemoryFault("CP0: performance overflow exceptions are not implemented");
                next[counter+1] += static_cast<std::uint32_t>(cycles);
            }
        }
        performance_ = next;
    }
    virtual std::uint64_t load(std::uint32_t address, unsigned size) = 0;
    virtual void store(std::uint32_t address, unsigned size, std::uint64_t value) = 0;
    virtual Register load_quad(std::uint32_t address) = 0;
    virtual void store_quad(std::uint32_t address, Register value) = 0;
    virtual void synchronize_cache(unsigned operation) = 0;
    virtual void advance_cycles(std::uint64_t cycles) = 0;
    virtual Register read_cop2_vector(unsigned) { throw MemoryFault("VU vectors unavailable on this bus"); }
    virtual void write_cop2_vector(unsigned, Register) { throw MemoryFault("VU vectors unavailable on this bus"); }
    virtual void add_subtract_cop2_vectors(unsigned, unsigned, unsigned, unsigned, bool, unsigned) {
        throw MemoryFault("VU arithmetic unavailable on this bus");
    }
    virtual std::uint32_t read_cop2_control(unsigned) { throw MemoryFault("VU control unavailable on this bus"); }
    virtual void write_cop2_control(unsigned, std::uint32_t) { throw MemoryFault("VU control unavailable on this bus"); }
    virtual void set_gs_crt(std::uint32_t, std::uint32_t, std::uint32_t) {
        throw MemoryFault("GS CRT services unavailable on this bus");
    }
    virtual bool interrupt_pending() const = 0;
    virtual std::uint32_t interrupt_lines() const { return interrupt_pending() ? 0x400U : 0U; }
    virtual void initialize_sif() { throw MemoryFault("SIF services unavailable on this bus"); }
    virtual std::uint32_t submit_sif(std::span<const SifTransfer>) { throw MemoryFault("SIF DMA unavailable on this bus"); }
    virtual std::int32_t sif_status(std::uint32_t) const { throw MemoryFault("SIF DMA unavailable on this bus"); }
private:
    // CPU-global device state survives thread and interrupt context restores.
    std::array<std::uint32_t,3> performance_{};
};
// Diagnostic memory, not a PS2 hardware emulator. No BIOS or devices are faked.
class RamBus final : public Bus {
public:
    RamBus();
    std::uint64_t load(std::uint32_t address, unsigned size) override;
    void store(std::uint32_t address, unsigned size, std::uint64_t value) override;
    Register load_quad(std::uint32_t address) override;
    void store_quad(std::uint32_t address, Register value) override;
    void install(std::uint32_t address, std::span<const std::uint8_t> data, std::uint32_t memory_size);
    void synchronize_cache(unsigned operation) override;
    void advance_cycles(std::uint64_t) override {}
    bool interrupt_pending() const override { return false; }
    std::span<const std::uint8_t> snapshot() const { return ram_; }
private:
    std::span<std::uint8_t> range(std::uint32_t address, unsigned size);
    std::vector<std::uint8_t> ram_;
    std::array<std::uint8_t,16 * 1024> scratchpad_{};
};
inline std::uint64_t sx32(std::uint32_t v) { return static_cast<std::uint64_t>(static_cast<std::int64_t>(std::bit_cast<std::int32_t>(v))); }
inline std::int64_t signed64(std::uint64_t v) { return std::bit_cast<std::int64_t>(v); }
Register padduw(Register a, Register b);
template<unsigned Bits, bool Upper>
inline Register interleave_lanes(Register a, Register b) {
    static_assert(Bits==8 || Bits==16 || Bits==32);
    const auto x=Upper ? a.hi : a.lo, y=Upper ? b.hi : b.lo;
    constexpr auto mask=(std::uint64_t(1)<<Bits)-1;
    Register result;
    for (unsigned lane=0;lane<64/Bits;++lane) {
        const auto pair=((y>>(lane*Bits))&mask) | (((x>>(lane*Bits))&mask)<<Bits);
        auto& half=lane*Bits*2<64 ? result.lo : result.hi;
        half |= pair << ((lane*Bits*2)%64);
    }
    return result;
}
template<unsigned Bits>
inline Register packed_equal(Register a, Register b) {
    static_assert(Bits == 8 || Bits == 16 || Bits == 32);
    constexpr auto mask = (std::uint64_t(1) << Bits) - 1;
    const auto half = [](std::uint64_t x, std::uint64_t y) {
        std::uint64_t result = 0;
        for (unsigned shift=0;shift<64;shift+=Bits)
            if (((x>>shift)&mask)==((y>>shift)&mask)) result |= mask<<shift;
        return result;
    };
    return {half(a.lo,b.lo),half(a.hi,b.hi)};
}
template<bool Expand>
inline Register packed_color5(Register value) {
    const auto word = [](std::uint32_t input) {
        std::uint32_t output = ((input >> (Expand ? 15 : 31)) & 1U) << (Expand ? 31 : 15);
        for (unsigned channel=0;channel<3;++channel) {
            const auto packed = channel*5, expanded = channel*8+3;
            output |= ((input >> (Expand ? packed : expanded)) & 31U) << (Expand ? expanded : packed);
        }
        return output;
    };
    const auto half = [&](std::uint64_t input) {
        return std::uint64_t(word(static_cast<std::uint32_t>(input))) | (std::uint64_t(word(static_cast<std::uint32_t>(input>>32)))<<32);
    };
    return {half(value.lo),half(value.hi)};
}
template<unsigned Bits>
inline Register pack_even_lanes(Register a, Register b) {
    static_assert(Bits == 8 || Bits == 16 || Bits == 32);
    const auto pack = [](Register value) {
        std::uint64_t result = 0;
        for (unsigned i=0;i<64/Bits;++i) {
            const auto source_bit = i*Bits*2;
            const auto half = source_bit < 64 ? value.lo : value.hi;
            result |= ((half >> (source_bit%64)) & ((std::uint64_t(1)<<Bits)-1)) << (i*Bits);
        }
        return result;
    };
    return {pack(b),pack(a)};
}
inline void divide_words(State& state, std::uint32_t numerator, std::uint32_t denominator, bool is_signed) {
    if (!denominator) {
        state.lo = is_signed && (numerator & 0x80000000U) ? 1 : ~std::uint64_t(0);
        state.hi = sx32(numerator);
    } else if (is_signed) {
        // Widen before division so INT32_MIN / -1 is defined in host C++.
        const auto a = static_cast<std::int64_t>(std::bit_cast<std::int32_t>(numerator));
        const auto b = static_cast<std::int64_t>(std::bit_cast<std::int32_t>(denominator));
        state.lo = sx32(static_cast<std::uint32_t>(a / b));
        state.hi = sx32(static_cast<std::uint32_t>(a % b));
    } else {
        state.lo = sx32(numerator / denominator);
        state.hi = sx32(numerator % denominator);
    }
}
using nt::merge_load;
inline std::uint64_t merge_double_load(std::uint64_t original, std::uint64_t memory, unsigned offset, bool left) {
    return merge_load(original,memory,offset,left);
}
inline std::uint64_t merge_word_load(std::uint64_t original, std::uint32_t memory, unsigned offset, bool left) {
    const auto value = merge_load(static_cast<std::uint32_t>(original),memory,offset,left);
    // EE LWR with a partial word preserves bits 32..63; LWL always sign extends.
    return left || !offset ? sx32(value) : (original & 0xffffffff00000000ULL) | value;
}
inline std::uint32_t merge_word_store(std::uint32_t memory, std::uint32_t value, unsigned offset, bool left) {
    return merge_load(memory,value,3-offset,!left);
}
inline std::uint64_t plzcw(std::uint64_t value) {
    const auto count = [](std::uint32_t word) {
        const auto normalized = (word & 0x80000000U) ? ~word : word;
        return static_cast<std::uint64_t>(std::countl_zero(normalized) - 1);
    };
    return count(static_cast<std::uint32_t>(value)) | (count(static_cast<std::uint32_t>(value >> 32)) << 32);
}
inline std::uint64_t merge_double_store(std::uint64_t memory, std::uint64_t value,
                                        unsigned offset, bool left) {
    // Stores perform the inverse byte selection of the complementary load.
    return merge_double_load(memory,value,7 - offset,!left);
}
using Entry = Result (*)(State&, Bus&, std::uint64_t);
}
