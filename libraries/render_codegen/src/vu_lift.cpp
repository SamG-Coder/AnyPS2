#include <nt/render_codegen.hpp>
#include <nt/vu_arithmetic.hpp>
#include <nt/vu_lower.hpp>
#include <array>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>

namespace nt::render {
namespace {
constexpr auto missing=std::numeric_limits<std::uint32_t>::max();
void check_lane(Lane lane,bool input) {
    if (lane.component>3 || (lane.file!=RegisterFile::vector && lane.file!=RegisterFile::accumulator) ||
        (lane.file==RegisterFile::vector ? lane.index>31 || (input && !lane.index) : lane.index!=0))
        throw std::invalid_argument("Invalid render kernel register lane");
}
bool no_lower_effect(const vu::Lower& operation) {
    using O=vu::LowerOperation;
    switch (operation.operation) {
    case O::move: case O::rotate:
        return operation.result.bank==vu::Bank::vector && (!operation.result.index || !operation.mask);
    case O::integer_add: case O::integer_subtract: case O::integer_add_immediate:
    case O::integer_subtract_immediate: case O::integer_and: case O::integer_or:
        return operation.result.bank==vu::Bank::integer && !operation.result.index;
    default: return false;
    }
}
[[noreturn]] void reject(unsigned address,const std::string& reason) {
    throw std::invalid_argument("Render region pair "+std::to_string(address)+": "+reason);
}
// Only pure nodes enter this pass. In particular, it must never be used to
// remove flag producers from a whole VU program without an external proof.
Kernel compact(Kernel source) {
    gpu::validate(source.arithmetic);
    const auto& old=source.arithmetic;
    std::vector<bool> live(old.inputs+old.expressions.size());
    for (auto output:old.outputs) live[output]=true;
    for (std::size_t n=old.expressions.size();n--;) {
        if (!live[old.inputs+n]) continue;
        const auto& expression=old.expressions[n];
        if (expression.operation!=gpu::Operation::constant)
            live[expression.left]=live[expression.right]=true;
    }
    Kernel result;
    result.outputs=source.outputs; result.source_pairs=source.source_pairs;
    std::vector<std::uint32_t> values(live.size(),missing);
    for (unsigned n=0;n<old.inputs;++n) if (live[n]) {
        values[n]=result.arithmetic.inputs++;
        result.inputs.push_back(source.inputs[n]);
    }
    using Key=std::tuple<gpu::Operation,std::uint32_t,std::uint32_t,std::uint32_t>;
    std::map<Key,std::uint32_t> common;
    for (std::size_t n=0;n<old.expressions.size();++n) {
        if (!live[old.inputs+n]) continue;
        auto expression=old.expressions[n];
        if (expression.operation==gpu::Operation::constant) expression.left=expression.right=0;
        else {
            expression.left=values[expression.left]; expression.right=values[expression.right];
            expression.bits=0;
        }
        const Key key{expression.operation,expression.left,expression.right,expression.bits};
        const auto next=result.arithmetic.inputs+static_cast<std::uint32_t>(result.arithmetic.expressions.size());
        const auto [found,inserted]=common.emplace(key,next);
        if (inserted) result.arithmetic.expressions.push_back(expression);
        values[old.inputs+n]=found->second;
    }
    for (auto output:old.outputs) result.arithmetic.outputs.push_back(values[output]);
    if (!result.arithmetic.inputs || result.arithmetic.expressions.empty())
        throw std::invalid_argument("Constant-only or passthrough region needs no arithmetic kernel");
    validate(result);
    return result;
}
}
void validate(const Kernel& kernel) {
    gpu::validate(kernel.arithmetic);
    if (kernel.inputs.size()!=kernel.arithmetic.inputs || kernel.outputs.size()!=kernel.arithmetic.outputs.size() ||
        kernel.inputs.size()>128 || kernel.outputs.size()>128 || !kernel.source_pairs || kernel.source_pairs>256)
        throw std::invalid_argument("Invalid render kernel interface");
    std::set<Lane> seen;
    for (const auto& input:kernel.inputs) {
        check_lane(input.source,true);
        if ((input.rate!=InputRate::vertex && input.rate!=InputRate::uniform) || !seen.insert(input.source).second)
            throw std::invalid_argument("Invalid or duplicated render kernel input");
    }
    for (const auto output:kernel.outputs) check_lane(output,false);
}
Layout layout(const Kernel& kernel) {
    validate(kernel);
    Layout result;
    for (const auto& input:kernel.inputs)
        ++(input.rate==InputRate::uniform ? result.uniform_words : result.vertex_words);
    result.output_words=static_cast<unsigned>(kernel.outputs.size());
    return result;
}
Kernel lift_vu_arithmetic(std::span<const vu::Pair> pairs,unsigned unit,
    std::span<const Lane> outputs,std::span<const unsigned> uniform_vectors) {
    if (unit>1 || pairs.empty() || pairs.size()>256 || outputs.empty() || outputs.size()>128)
        throw std::invalid_argument("Render lifting requires 1-256 pairs and 1-128 output lanes");
    const auto capacity=unit ? 2048U : 512U;
    if (pairs.front().address>=capacity || pairs.size()>capacity-pairs.front().address)
        throw std::invalid_argument("Render region crosses VU instruction storage");
    std::array<bool,32> uniforms{};
    for (auto index:uniform_vectors) {
        if (!index || index>31) throw std::invalid_argument("Uniform vector must be vf1-vf31");
        uniforms[index]=true;
    }
    Kernel result;
    result.source_pairs=static_cast<unsigned>(pairs.size());
    result.outputs.assign(outputs.begin(),outputs.end());
    std::array<std::array<std::uint32_t,4>,32> vectors{};
    std::array<std::uint32_t,4> accumulator{};
    auto& program=result.arithmetic;
    for (unsigned index=1;index<32;++index) for (unsigned lane=0;lane<4;++lane) {
        vectors[index][lane]=program.inputs++;
        result.inputs.push_back({{RegisterFile::vector,index,lane},uniforms[index] ? InputRate::uniform : InputRate::vertex});
    }
    for (unsigned lane=0;lane<4;++lane) {
        accumulator[lane]=program.inputs++;
        result.inputs.push_back({{RegisterFile::accumulator,0,lane},InputRate::vertex});
    }
    const auto append=[&](gpu::Operation operation,std::uint32_t left=0,std::uint32_t right=0,std::uint32_t bits=0) {
        const auto value=program.inputs+static_cast<std::uint32_t>(program.expressions.size());
        program.expressions.push_back({operation,left,right,bits}); return value;
    };
    const auto zero=append(gpu::Operation::constant),one=append(gpu::Operation::constant,0,0,0x3f800000U);
    vectors[0]={zero,zero,zero,one};
    for (std::size_t n=0;n<pairs.size();++n) {
        const auto& pair=pairs[n];
        if (pair.address!=pairs.front().address+n) reject(pair.address,"noncontiguous region");
        if (pair.end || pair.interlock || pair.debug || pair.trap || pair.immediate)
            reject(pair.address,"control flags or immediate publication require a larger dependency proof");
        const auto lower=vu::decode_lower(pair,unit);
        if (!no_lower_effect(lower)) reject(pair.address,"lower operation "+std::string(vu::name(lower.operation))+" is not a proven no-op");
        const auto upper=vu::decode_upper(pair.upper);
        if (upper.operation==vu::Arithmetic::nop) continue;
        using A=vu::Arithmetic;
        switch (upper.operation) {
        case A::add: case A::subtract: case A::multiply: case A::multiply_add: case A::multiply_subtract: break;
        default: reject(pair.address,"unsupported arithmetic region operation "+std::string(vu::name(upper.operation)));
        }
        if (upper.left>31 || upper.right>31 || upper.result>31 || upper.mask>15 ||
            (upper.destination!=vu::Destination::vector && upper.destination!=vu::Destination::accumulator))
            reject(pair.address,"invalid arithmetic fields");
        if (upper.operand!=vu::Operand::vector &&
            (upper.operand<vu::Operand::x || upper.operand>vu::Operand::w))
            reject(pair.address,"scalar I/Q input needs publication analysis");
        if (upper.destination==vu::Destination::vector && !upper.result) continue;
        // Snapshot sources before committing any lane, including broadcasts
        // from a destination register that this instruction also overwrites.
        const auto left=vectors[upper.left],right=vectors[upper.right],previous=accumulator;
        auto value=upper.destination==vu::Destination::accumulator ? accumulator : vectors[upper.result];
        for (unsigned lane=0;lane<4;++lane) {
            if (!(upper.mask&(8U>>lane))) continue;
            const auto rhs=right[upper.operand==vu::Operand::vector ? lane :
                static_cast<unsigned>(upper.operand)-static_cast<unsigned>(vu::Operand::x)];
            switch (upper.operation) {
            case A::add: value[lane]=append(gpu::Operation::add,left[lane],rhs); break;
            case A::subtract: value[lane]=append(gpu::Operation::subtract,left[lane],rhs); break;
            case A::multiply: value[lane]=append(gpu::Operation::multiply,left[lane],rhs); break;
            case A::multiply_add: case A::multiply_subtract: {
                const auto product=append(gpu::Operation::multiply,left[lane],rhs);
                value[lane]=append(upper.operation==A::multiply_add ? gpu::Operation::add : gpu::Operation::subtract,
                    previous[lane],product); break;
            }
            default: break;
            }
        }
        if (upper.destination==vu::Destination::accumulator) accumulator=value;
        else vectors[upper.result]=value;
    }
    for (const auto output:outputs) {
        check_lane(output,false);
        program.outputs.push_back(output.file==RegisterFile::accumulator ? accumulator[output.component] : vectors[output.index][output.component]);
    }
    return compact(std::move(result));
}
}
