#include <nt/vector_region.hpp>
#include <nt/element_abi.hpp>
#include <algorithm>
#include <cstring>
#include <string>
#include <stdexcept>

namespace nt::render {
namespace {
bool quiescent(const vu::State& s) {
    return !s.delayed_integer && !s.quotient_pending && !s.p_pending && !s.mac_read_delay &&
        !s.clip_read_delay && !s.quotient_read_delay && !s.p_read_delay && !s.pending_mac_mask &&
        !s.pending_clip_mask && !s.graphics_pending;
}
ElementEntry entry_of(const vu::State& s) {
    ElementEntry entry;
    std::copy(s.integers.begin(),s.integers.end(),entry.integers.begin()+1);
    std::copy(s.vectors.begin(),s.vectors.end(),entry.vectors.begin()+1);
    entry.vectors[0][3]=0x3f800000;
    entry.accumulator=s.accumulator; entry.immediate=s.immediate; entry.quotient=s.quotient;
    entry.p=s.scalar_p; entry.top=s.input_top; entry.itop=s.input_itop;
    return entry;
}
void iteration(const VectorRegion& region,vu::State& s,bool last) {
    if (region.iteration(&s)!=vu::Result::transfer || s.next_pair!=(last ? region.exit:region.head))
        throw std::runtime_error("Vector region boundary execution did not reach its compiled continuation");
}
}
RegionExecution execute_vector_region(const VectorRegion& region,vu::State& state,RegionExecutor executor,void* context) {
    if (region.unit>1 || !region.iteration || !region.guard || !region.kernel || !region.counter || region.counter>=16 ||
        !region.step || !region.stores || region.stores>64 || !region.drain || region.drain>8 || region.startup>8)
        throw std::invalid_argument("Invalid compiled vector region descriptor");
    if (state.next_pair!=region.head || !quiescent(state))
        throw std::invalid_argument("Vector region requires its declared entry and quiescent pipelines");
    const auto entry=entry_of(state);
    const unsigned count=entry.integers[region.counter]/region.step;
    auto parameters=element_parameters(entry,0,count);
    if (const auto failure=region.guard(parameters.data())) {
        // Values are the compiled VectorRangeStatus ABI shared with draw launches.
        constexpr const char* reasons[]{"valid", "invalid countdown or element count", "unproved startup range",
            "integer or memory range wraps", "input/output memory aliases"};
        throw std::invalid_argument(std::string("Vector region entry rejected: ")+
            (failure<std::size(reasons) ? reasons[failure]:"unknown range-guard status"));
    }
    auto result=state;
    RegionExecution execution;
    if (count<=region.startup+region.drain) {
        for (unsigned i=0;i<count;++i) iteration(region,result,i+1==count);
        execution.startup=count;
    } else {
        // Startup stores may depend on incoming VF lanes; use their original AOT code.
        for (unsigned i=0;i<region.startup;++i) iteration(region,result,false);
        execution.startup=region.startup;
        execution.elements=count-region.startup;
        parameters[0]=execution.elements; parameters[1]=region.startup;
        const unsigned words=region.stores+1;
        std::vector<std::uint32_t> output(std::size_t(execution.elements)*words,0xffffffffU);
        std::array<std::uint32_t,4096> input;
        std::memcpy(input.data(),state.data.data(),sizeof(input));
        const auto data=std::span<const std::uint32_t>(input).first(std::size_t(region.unit ? 1024:256)*4);
        if (executor) executor(context,data,parameters,output);
        else region.kernel(data.data(),parameters.data(),output.data());
        for (unsigned i=0;i<execution.elements;++i)
            if (output[std::size_t(i)*words]) throw std::runtime_error("Vector region element kernel reported unsupported behavior");
        // Replay only the proven overwrite horizon to reconstruct all architectural
        // live-out lanes. Its transient stores are replaced by recovered outputs.
        auto tail=state;
        const auto skipped=count-region.drain;
        for (unsigned i=1;i<16;++i) {
            const auto value=std::int64_t(entry.integers[i])+std::int64_t(region.strides[i])*skipped;
            if (value<0 || value>65535) throw std::logic_error("Vector region guard did not bound integer state");
            tail.integers[i-1]=static_cast<std::uint16_t>(value);
        }
        for (unsigned i=0;i<region.drain;++i) iteration(region,tail,i+1==region.drain);
        tail.data=result.data; // retain the genuine startup and untouched memory
        for (unsigned i=0;i<execution.elements;++i) for (unsigned n=0;n<region.stores;++n) {
            const auto& store=region.output[n];
            if (!store.base || store.base>=16 || store.lane>=4) throw std::logic_error("Invalid vector region store binding");
            const auto row=std::int64_t(entry.integers[store.base])+store.row+
                std::int64_t(store.stride)*(region.startup+i);
            if (row<0 || row>=(region.unit ? 1024:256)) throw std::logic_error("Vector region guard did not bound output memory");
            tail.data[static_cast<unsigned>(row)][store.lane]=output[std::size_t(i)*words+1+n];
        }
        result=tail; execution.drain=region.drain;
    }
    state=result;
    return execution;
}
}
