#include <nt/render_loop.hpp>
#include <nt/vu_arithmetic.hpp>
#include <nt/vu_lower.hpp>
#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>

namespace nt::render {
namespace {
using O=vu::LowerOperation;
[[noreturn]] void reject(unsigned address,const char* reason) {
    throw std::invalid_argument("Vertex loop pair "+std::to_string(address)+": "+reason);
}
bool lower_nop(const vu::Lower& lower) {
    // Same canonical register-zero MOVE accepted by the arithmetic lifter.
    return lower.operation==O::move && lower.result.bank==vu::Bank::vector && !lower.result.index;
}
const VertexStream& origin(const VertexLoop& loop,Lane lane) {
    const auto found=std::find_if(loop.reads().begin(),loop.reads().end(),[&](const auto& read) {
        return lane.file==RegisterFile::vector && read.vector==lane.index;
    });
    if (found==loop.reads().end())
        throw std::invalid_argument("Vertex loop contains a live input without a per-iteration load");
    return *found;
}
}
VertexLoop analyze_vertex_loop(std::span<const vu::Pair> pairs,unsigned unit) {
    if (unit>1 || pairs.size()<7 || pairs.size()>256)
        throw std::invalid_argument("Vertex loop requires 7-256 pairs and a valid VU unit");
    const unsigned capacity=unit ? 2048 : 512;
    if (pairs.front().address>=capacity || pairs.size()>capacity-pairs.front().address)
        throw std::invalid_argument("Vertex loop crosses instruction storage");
    for (std::size_t n=0;n<pairs.size();++n) {
        const auto& p=pairs[n];
        if (p.address!=pairs.front().address+n || p.end || p.interlock || p.debug || p.trap || p.immediate)
            reject(p.address,"noncontiguous code, immediate publication or control flags");
    }
    const auto branch=vu::control(pairs[pairs.size()-2],unit);
    if (branch.branch!=vu::Branch::not_equal || !branch.target || *branch.target!=pairs.front().address ||
        (branch.source==0)==(branch.target_register==0))
        reject(pairs[pairs.size()-2].address,"expected a single countdown backedge comparing against vi0");
    VertexLoop result;
    result.unit_=unit; result.pairs_=static_cast<unsigned>(pairs.size());
    result.counter_=branch.source ? branch.source : branch.target_register;
    for (std::size_t n=pairs.size()-2;n<pairs.size();++n) {
        if (vu::decode_upper(pairs[n].upper).operation!=vu::Arithmetic::nop ||
            (n==pairs.size()-1 && !lower_nop(vu::decode_lower(pairs[n],unit))))
            reject(pairs[n].address,"branch upper operation or delay slot has effects");
    }
    enum class Phase { loads, arithmetic, updates };
    auto phase=Phase::loads;
    std::size_t first_math=0,last_math=0,last_load=0,counter_update=0;
    std::array<bool,32> loaded{};
    std::array<bool,16> updated{};
    bool stored=false,math=false;
    for (std::size_t n=0;n+2<pairs.size();++n) {
        const auto& pair=pairs[n];
        const auto upper=vu::decode_upper(pair.upper);
        const auto lower=vu::decode_lower(pair,unit);
        if (upper.operation!=vu::Arithmetic::nop) {
            if (phase==Phase::updates || !lower_nop(lower))
                reject(pair.address,"arithmetic overlaps memory/control work or follows the store");
            if (!math) first_math=n;
            math=true; last_math=n; phase=Phase::arithmetic;
            continue;
        }
        if (lower_nop(lower)) continue;
        if (lower.operation==O::load_quad) {
            if (phase!=Phase::loads || lower.address_update!=vu::AddressUpdate::none || lower.mask!=15 ||
                !lower.result.index || lower.result.index>31 || loaded[lower.result.index])
                reject(pair.address,"loads must define distinct full vectors before arithmetic, without address updates");
            if (lower.address_register==result.counter_)
                reject(pair.address,"countdown register also addresses memory");
            loaded[lower.result.index]=true; last_load=n;
            result.reads_.push_back({lower.result.index,lower.address_register,lower.offset,0});
            continue;
        }
        if (lower.operation==O::store_quad) {
            if (phase!=Phase::arithmetic || stored || lower.address_update!=vu::AddressUpdate::none ||
                lower.mask!=15 || !lower.left.index || lower.left.index>31 || !lower.address_register ||
                lower.address_register==result.counter_)
                reject(pair.address,"expected one full-vector store after arithmetic");
            stored=true; phase=Phase::updates;
            result.write_={lower.left.index,lower.address_register,lower.offset,0};
            continue;
        }
        if (lower.operation==O::integer_add_immediate || lower.operation==O::integer_subtract_immediate) {
            const auto index=lower.result.index;
            if (phase!=Phase::updates || !index || index>15 || lower.left.index!=index || updated[index])
                reject(pair.address,"induction must update each nonzero integer at most once after the store");
            const auto step=lower.operation==O::integer_subtract_immediate ? -lower.offset : lower.offset;
            if ((index==result.counter_ && step!=-1) || (index!=result.counter_ && (step<1 || step>1024)))
                reject(pair.address,"expected countdown by one and bounded forward stream strides");
            updated[index]=true; result.steps_[index]=step;
            if (index==result.counter_) counter_update=n;
            continue;
        }
        reject(pair.address,"unsupported memory, flag, scalar, packet or control effect");
    }
    if (!math || !stored || result.reads_.empty() || !updated[result.counter_])
        throw std::invalid_argument("Vertex loop is missing loads, arithmetic, store or countdown");
    if (first_math<last_load+2)
        throw std::invalid_argument("Vertex loop loads need a neutral pair before arithmetic");
    // Do not rely on a just-written IALU value at the branch. The deliberately
    // narrow form has two neutral pairs after all induction updates.
    if (pairs.size()<5 || counter_update+3>pairs.size()-2)
        throw std::invalid_argument("Vertex loop countdown needs separation before the branch");
    for (std::size_t n=pairs.size()-4;n<pairs.size()-2;++n)
        if (vu::decode_upper(pairs[n].upper).operation!=vu::Arithmetic::nop || !lower_nop(vu::decode_lower(pairs[n],unit)))
            reject(pairs[n].address,"expected two neutral pairs before the backedge");
    result.write_.stride_rows=static_cast<unsigned>(result.steps_[result.write_.base_register]);
    if (!result.write_.stride_rows) throw std::invalid_argument("Vertex loop overwrites one output row repeatedly");
    std::array<bool,16> used{};
    used[result.counter_]=used[result.write_.base_register]=true;
    std::vector<unsigned> uniforms;
    for (auto& read:result.reads_) {
        used[read.base_register]=true;
        read.stride_rows=static_cast<unsigned>(result.steps_[read.base_register]);
        if (!read.stride_rows) uniforms.push_back(read.vector);
    }
    for (unsigned n=1;n<16;++n) if (updated[n] && !used[n])
        throw std::invalid_argument("Vertex loop updates an unrelated integer register");
    std::array<Lane,4> outputs{};
    for (unsigned n=0;n<4;++n) outputs[n]={RegisterFile::vector,result.write_.vector,n};
    result.kernel_=lift_vu_arithmetic(pairs.subspan(first_math,last_math-first_math+1),unit,outputs,uniforms);
    // A remaining entry accumulator/vector dependency would couple iterations
    // or require additional caller state. Every live source must be loaded here.
    for (const auto& input:result.kernel_.inputs) (void)origin(result,input.source);
    return result;
}
LoopBindings bind_vertex_loop(const VertexLoop& loop,std::span<const std::uint16_t,16> integers,
    std::span<const std::uint32_t> source) {
    const unsigned capacity=loop.unit() ? 1024 : 256;
    if (source.empty() || source.size()%4 || source.size()>capacity*4 || integers[0])
        throw std::invalid_argument("Vertex loop source extent or constant integer register is invalid");
    const unsigned count=integers[loop.counter()];
    // The original loop is do/while: zero is NOT an empty batch.
    if (!count || count>capacity) throw std::invalid_argument("Vertex loop count is zero or exceeds bounded output capacity");
    for (unsigned n=1;n<16;++n) if (loop.steps()[n]) {
        const auto final=std::int64_t(integers[n])+std::int64_t(loop.steps()[n])*count;
        if (final<0 || final>65535) throw std::invalid_argument("Vertex loop integer induction would wrap");
    }
    struct Range { unsigned first,last; };
    const auto range=[&](const VertexStream& stream) {
        const auto first=std::int64_t(integers[stream.base_register])+stream.offset_rows;
        const auto last=first+std::int64_t(count-1)*stream.stride_rows;
        if (first<0 || last>=static_cast<std::int64_t>(source.size()/4))
            throw std::invalid_argument("Vertex loop memory extent would wrap or exceed the captured source");
        return Range{static_cast<unsigned>(first),static_cast<unsigned>(last)};
    };
    const auto output=range(loop.write());
    for (const auto& read:loop.reads()) {
        const auto input=range(read);
        // Conservative envelopes intentionally reject interleaved aliasing too.
        if (input.first<=output.last && output.first<=input.last)
            throw std::invalid_argument("Vertex loop output overlaps an input or uniform extent");
    }
    LoopBindings result;
    result.output_words=count*4; result.source_output_row=output.first;
    result.source_output_stride=loop.write().stride_rows;
    result.parameters.reserve(1+loop.kernel().inputs.size());
    result.parameters.push_back(count);
    for (const auto& input:loop.kernel().inputs) {
        const auto& read=origin(loop,input.source);
        const auto word=range(read).first*4+input.source.component;
        result.parameters.push_back(read.stride_rows ? word : source[word]);
    }
    return result;
}
void write_manifest(std::ostream& out,const VertexLoop& loop) {
    out<<"{\n  \"schema\": \"anyps2.vertex-loop.v1\",\n  \"installable\": false,\n"
       <<"  \"numeric_policy\": \"native-float32\",\n  \"source_pairs\": "<<loop.source_pairs()
       <<",\n  \"counter_register\": "<<loop.counter()<<",\n  \"parameter_words\": "<<loop.kernel().inputs.size()+1
       <<",\n  \"output_words_per_item\": 4,\n  \"reads\": [\n";
    for (unsigned n=0;n<loop.reads().size();++n) {
        const auto& read=loop.reads()[n];
        out<<"    {\"vector\": "<<read.vector<<", \"base_register\": "<<read.base_register
           <<", \"offset_rows\": "<<read.offset_rows<<", \"stride_rows\": "<<read.stride_rows
           <<", \"rate\": \""<<(read.stride_rows ? "vertex" : "uniform")<<"\"}"
           <<(n+1==loop.reads().size() ? "\n" : ",\n");
    }
    out<<"  ],\n  \"parameters\": [\n";
    for (unsigned n=0;n<loop.kernel().inputs.size();++n) {
        const auto& input=loop.kernel().inputs[n]; const auto& read=origin(loop,input.source);
        out<<"    {\"word\": "<<n+1<<", \"vector\": "<<input.source.index
           <<", \"component\": "<<input.source.component<<", \"kind\": \""
           <<(read.stride_rows ? "source-word-offset" : "float32-bits")<<"\"}"
           <<(n+1==loop.kernel().inputs.size() ? "\n" : ",\n");
    }
    const auto& write=loop.write();
    out<<"  ],\n  \"source_output\": {\"base_register\": "<<write.base_register
       <<", \"offset_rows\": "<<write.offset_rows<<", \"stride_rows\": "<<write.stride_rows
       <<"},\n  \"requires\": [\"bind-time extent and non-alias guards\", "
       <<"\"external register/flag liveness and region-entry proof\", "
       <<"\"native-float32 acceptance\", \"validated SPIR-V and Vulkan execution\", "
       <<"\"resource ownership and draw integration\"]\n}\n";
    if (!out) throw std::runtime_error("Cannot emit vertex loop manifest");
}
}
