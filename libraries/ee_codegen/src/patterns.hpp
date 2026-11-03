#pragma once
#include <nt/r5900.hpp>
#include <bit>
#include <optional>
#include <span>

namespace nt::ee_codegen {
// AOT-only recognition of a side-effect-free VSYNC poll. Keep all original
// entries and the actual runtime address; other loads retain ordinary code.
inline bool display_wait_pattern(std::span<const r5900::Instruction> code) {
    using r5900::Op;
    if (code.size()<4) return false;
    const auto& load=code[0]; const auto& mask=code[1]; const auto& branch=code[2];
    for (unsigned n=1;n<4;++n) if (std::uint64_t(load.pc)+n*4!=code[n].pc) return false;
    return (load.op==Op::ld || load.op==Op::lw || load.op==Op::lwu) && load.rt && load.rs!=load.rt &&
        mask.op==Op::andi && mask.rs==load.rt && mask.rt==load.rt && mask.immediate==8 &&
        (branch.op==Op::beq || branch.op==Op::beql) &&
        ((branch.rs==load.rt && !branch.rt) || (!branch.rs && branch.rt==load.rt)) &&
        branch.immediate==-3 && code[3].word==0;
}
// Constant/address materialization. Each original entry remains independently
// executable; this describes only a straight-line, two-operation fast path.
struct ConstantPattern { unsigned destination; std::uint64_t value; };
inline std::optional<ConstantPattern> constant_pattern(const r5900::Instruction& first,const r5900::Instruction& second) {
    using r5900::Op;
    if (first.op!=Op::lui || second.pc!=std::uint64_t(first.pc)+4 ||
        second.rt!=first.rt || second.rs!=first.rt) return {};
    const auto upper=static_cast<std::uint32_t>(first.immediate)&65535U;
    const auto base=upper<<16;
    const auto extend=[](std::uint32_t value) {
        return static_cast<std::uint64_t>(static_cast<std::int64_t>(std::bit_cast<std::int32_t>(value)));
    };
    if (second.op==Op::ori) return ConstantPattern{first.rt,extend(base)|(static_cast<std::uint32_t>(second.immediate)&65535U)};
    const auto delta=static_cast<std::uint64_t>(static_cast<std::int64_t>(second.immediate));
    if (second.op==Op::addiu) return ConstantPattern{first.rt,extend(base+static_cast<std::uint32_t>(delta))};
    if (second.op==Op::daddiu) return ConstantPattern{first.rt,extend(base)+delta};
    return {};
}
}
