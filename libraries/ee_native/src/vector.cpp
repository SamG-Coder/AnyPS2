#include <nt/ee_native.hpp>
#include <nt/ee_vector_math.hpp>
#include <nt/vu_quotient.hpp>

extern "C" std::uint32_t nt_ee_vector_memory(nt::ee::Memory* memory,std::uint32_t unit) {
    using nt::ee::Stop;
    if (unit>1) return static_cast<std::uint32_t>(Stop::invalid_state);
    const auto& region=memory->vector_data[unit];
    const auto result=!region.data ? Stop::unsupported_vector :
        region.active && *region.active ? Stop::vector_pending : Stop::budget;
    return static_cast<std::uint32_t>(result);
}

extern "C" std::uint32_t nt_ee_vector_launch(nt::ee::Memory* memory,std::uint32_t indirect,std::uint32_t entry) {
    if (indirect>1) return static_cast<std::uint32_t>(nt::ee::Stop::invalid_state);
    const auto result=memory->launch_vector ? memory->launch_vector(memory->vector_launch_context,indirect!=0,entry) :
        nt::ee::Stop::unsupported_vector;
    return static_cast<std::uint32_t>(result);
}

extern "C" std::uint64_t nt_ee_vector_read(nt::ee::Memory* memory,std::uint32_t selector,std::uint32_t wait) {
    using nt::ee::VectorStatus;
    auto result=nt::ee::VectorReadResult{VectorStatus::unsupported};
    if (selector<32 && wait<=1 && memory->vector_control.read)
        result=memory->vector_control.read(memory->vector_control.context,selector,wait!=0);
    if (result.status!=VectorStatus::ready && result.status!=VectorStatus::pending)
        result={VectorStatus::unsupported};
    return (std::uint64_t(result.status)<<32)|result.value;
}

extern "C" std::uint32_t nt_ee_vector_quotient(nt::ee::Memory* memory,std::uint32_t left,
        std::uint32_t right,std::uint32_t left_lane,std::uint32_t right_lane,std::uint32_t operation) {
    using nt::ee::Stop;
    auto* vectors=memory->vectors;
    if (!vectors) return std::uint32_t(Stop::unsupported_vector);
    if (vectors->active) return std::uint32_t(Stop::vector_pending);
    if (left>31 || right>31 || left_lane>3 || right_lane>3 || operation>2) return std::uint32_t(Stop::invalid_state);
    const auto read=[&](unsigned reg,unsigned lane) { return reg ? vectors->values[reg-1][lane] : lane==3 ? 0x3f800000U : 0U; };
    const auto a=read(left,left_lane),b=read(right,right_lane);
    const auto result=operation==0 ? nt::numeric::vu_divide_finite(a,b) : operation==1 ?
        nt::numeric::vu_square_root_finite(b) : nt::numeric::vu_reciprocal_square_root_finite(a,b);
    if (!result.supported) return std::uint32_t(Stop::unsupported_numeric);
    vectors->quotient=result.bits;
    vectors->status=(vectors->status&0xfcfU)|result.flags|(result.flags<<6);
    return std::uint32_t(Stop::budget);
}

extern "C" std::uint32_t nt_ee_vector_outer(nt::ee::Memory* memory,std::uint32_t destination,
        std::uint32_t left,std::uint32_t right,std::uint32_t subtract) {
    using nt::ee::Stop;
    auto* vectors=memory->vectors;
    if (!vectors) return std::uint32_t(Stop::unsupported_vector);
    if (vectors->active) return std::uint32_t(Stop::vector_pending);
    if (destination>32 || left>31 || right>31 || subtract>1) return std::uint32_t(Stop::invalid_state);
    const auto read=[&](unsigned reg) { return reg==32 ? vectors->accumulator : reg ? vectors->values[reg-1] : nt::numeric::Vector{0,0,0,0x3f800000}; };
    const auto a=read(left),b=read(right);
    const nt::numeric::Vector rotated_a{a[1],a[2],a[0],0},rotated_b{b[2],b[0],b[1],0};
    auto result=nt::numeric::binary_vector(read(destination),rotated_a,rotated_b,14,4,vectors->status,
        [&](auto first,auto second,unsigned lane) { return subtract ?
            nt::numeric::multiply_accumulate_finite(vectors->accumulator[lane],first,second,true) :
            nt::numeric::multiply_finite(first,second); });
    if (!result.supported) return std::uint32_t(Stop::unsupported_numeric);
    result.mac|=vectors->mac&0x1111U;
    if (destination==32) vectors->accumulator=result.value;
    else if (destination) vectors->values[destination-1]=result.value;
    vectors->mac=result.mac; vectors->status=nt::numeric::vector_status_flags(result.mac,vectors->status);
    return std::uint32_t(Stop::budget);
}

extern "C" std::uint64_t nt_ee_vector_write(nt::ee::Memory* memory,std::uint32_t selector,std::uint32_t wait,std::uint32_t value) {
    using nt::ee::VectorStatus;
    auto status=VectorStatus::unsupported;
    if (selector<32 && wait<=1 && memory->vector_control.write)
        status=memory->vector_control.write(memory->vector_control.context,selector,wait!=0,value);
    if (status!=VectorStatus::ready && status!=VectorStatus::pending)
        status=VectorStatus::unsupported;
    return std::uint64_t(status)<<32;
}

namespace {
template<class Arithmetic>
std::uint32_t vector_arithmetic(nt::ee::Memory* memory,std::uint32_t destination,std::uint32_t left,
                                std::uint32_t right,std::uint32_t mask,std::uint32_t broadcast,Arithmetic arithmetic) {
    using nt::ee::Stop;
    auto* vectors=memory->vectors;
    if (!vectors) return std::uint32_t(Stop::unsupported_vector);
    if (vectors->active) return std::uint32_t(Stop::vector_pending);
    // Native destination 32 denotes the shared accumulator; no guest register
    // decoding occurs in this arithmetic service.
    if (destination>32 || left>=32 || right>33 || mask>15 || broadcast>4)
        return std::uint32_t(Stop::invalid_state);
    const auto read=[&](unsigned r) { return r==32 ? vectors->accumulator : r ? vectors->values[r-1] : nt::numeric::Vector{0,0,0,0x3f800000}; };
    auto operand=right<32 ? read(right) : nt::numeric::Vector{};
    if (right>=32) operand.fill(right==32 ? vectors->quotient : vectors->immediate);
    const auto result=nt::numeric::binary_vector(read(destination),read(left),operand,mask,broadcast,vectors->status,arithmetic);
    if (!result.supported) return std::uint32_t(Stop::unsupported_numeric);
    if (destination==32) vectors->accumulator=result.value;
    else if (destination) vectors->values[destination-1]=result.value;
    vectors->mac=result.mac; vectors->status=result.status;
    return std::uint32_t(Stop::budget);
}
}
extern "C" std::uint32_t nt_ee_vector_add(nt::ee::Memory* memory,std::uint32_t destination,std::uint32_t left,
                                         std::uint32_t right,std::uint32_t mask,std::uint32_t subtract,std::uint32_t broadcast) {
    if (subtract>1) return std::uint32_t(nt::ee::Stop::invalid_state);
    return vector_arithmetic(memory,destination,left,right,mask,broadcast,
        [subtract](auto a,auto b,unsigned) { return nt::numeric::add_subtract_finite(a,b,subtract!=0); });
}
extern "C" std::uint32_t nt_ee_vector_multiply(nt::ee::Memory* memory,std::uint32_t destination,std::uint32_t left,
                                              std::uint32_t right,std::uint32_t mask,std::uint32_t broadcast) {
    return vector_arithmetic(memory,destination,left,right,mask,broadcast,[](auto a,auto b,unsigned) { return nt::numeric::multiply_finite(a,b); });
}

extern "C" std::uint32_t nt_ee_vector_multiply_accumulate(nt::ee::Memory* memory,std::uint32_t destination,std::uint32_t left,
        std::uint32_t right,std::uint32_t mask,std::uint32_t subtract,std::uint32_t broadcast) {
    if (subtract>1) return std::uint32_t(nt::ee::Stop::invalid_state);
    return vector_arithmetic(memory,destination,left,right,mask,broadcast,
        [memory,subtract](auto a,auto b,unsigned lane) {
            return nt::numeric::multiply_accumulate_finite(memory->vectors->accumulator[lane],a,b,subtract!=0);
        });
}
