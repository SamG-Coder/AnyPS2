#pragma once
#include <array>
#include <bit>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace nt::runtime {
struct Register { std::uint64_t lo = 0, hi = 0; };
struct State {
    std::array<Register, 32> gpr{};
    std::array<std::uint32_t, 32> fpr{};
    std::uint32_t fcr31 = 0, accumulator = 0;
    std::uint32_t cop0_status = 0;
    std::uint32_t cop0_epc = 0, cop0_error_epc = 0;
    std::uint64_t hi = 0, lo = 0, executed = 0;
    std::uint64_t hi1 = 0, lo1 = 0, sa = 0;
    std::uint32_t pc = 0, pending_target = 0, branch_pc = 0;
    bool delay_slot = false;
    void set(unsigned reg, std::uint64_t value) { if (reg) gpr[reg].lo = value; }
    void set128(unsigned reg, Register value) { if (reg) gpr[reg] = value; }
};
enum class Stop { budget, syscall, breakpoint, unsupported, unmapped_code, code_changed, memory_fault, branch_in_delay, interrupt_pending };
struct Result { Stop reason; std::uint32_t pc, word; std::string detail{}; };
const char* name(Stop stop);
class MemoryFault : public std::runtime_error { public: using std::runtime_error::runtime_error; };
// Translate only the BIOS-free default RAM aliases; callers enforce RAM bounds.
std::uint32_t default_ram_address(std::uint32_t address);
struct SifTransfer { std::uint32_t source, destination, bytes, attributes; };
// The generated code uses this boundary; a future PCSX2 adapter owns MMIO and timing.
class Bus {
public:
    virtual ~Bus() = default;
    virtual std::uint64_t load(std::uint32_t address, unsigned size) = 0;
    virtual void store(std::uint32_t address, unsigned size, std::uint64_t value) = 0;
    virtual Register load_quad(std::uint32_t address) = 0;
    virtual void store_quad(std::uint32_t address, Register value) = 0;
    virtual void synchronize_cache(unsigned operation) = 0;
    virtual void advance_cycles(std::uint64_t cycles) = 0;
    virtual void set_gs_crt(std::uint32_t, std::uint32_t, std::uint32_t) {
        throw MemoryFault("GS CRT services unavailable on this bus");
    }
    virtual bool interrupt_pending() const = 0;
    virtual std::uint32_t interrupt_lines() const { return interrupt_pending() ? 0x400U : 0U; }
    virtual void initialize_sif() { throw MemoryFault("SIF services unavailable on this bus"); }
    virtual std::uint32_t submit_sif(std::span<const SifTransfer>) { throw MemoryFault("SIF DMA unavailable on this bus"); }
    virtual std::int32_t sif_status(std::uint32_t) const { throw MemoryFault("SIF DMA unavailable on this bus"); }
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
template<class Word>
inline Word merge_load(Word original, Word memory, unsigned offset, bool left) {
    static_assert(std::is_unsigned_v<Word> && (sizeof(Word) == 4 || sizeof(Word) == 8));
    constexpr auto bits = sizeof(Word) * 8;
    const auto shift = (left ? sizeof(Word) - 1 - offset : offset) * 8;
    if (!shift) return memory;
    if (left) return (original & ((Word(1) << shift) - 1)) | (memory << shift);
    return (original & (Word(~Word(0)) << (bits - shift))) | (memory >> shift);
}
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
