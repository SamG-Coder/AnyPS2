#pragma once
#include <nt/r5900.hpp>
#include <bit>
#include <optional>

namespace nt::ee_codegen {
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
