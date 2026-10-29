#include <nt/ee_native.hpp>
#include <nt/ee_vector_math.hpp>

extern "C" std::uint64_t nt_ee_vector_read(nt::ee::Memory* memory,std::uint32_t selector,std::uint32_t wait) {
    using nt::ee::VectorStatus;
    auto result=nt::ee::VectorReadResult{VectorStatus::unsupported};
    if (selector<32 && selector!=20 && wait<=1 && memory->vector_control.read)
        result=memory->vector_control.read(memory->vector_control.context,selector,wait!=0);
    if (result.status!=VectorStatus::ready && result.status!=VectorStatus::pending)
        result={VectorStatus::unsupported};
    return (std::uint64_t(result.status)<<32)|result.value;
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
    if (destination>=32 || left>=32 || right>=32 || mask>15 || broadcast>4)
        return std::uint32_t(Stop::invalid_state);
    const auto read=[&](unsigned r) { return r ? vectors->values[r-1] : nt::numeric::Vector{0,0,0,0x3f800000}; };
    const auto result=nt::numeric::binary_vector(read(destination),read(left),read(right),mask,broadcast,vectors->status,arithmetic);
    if (!result.supported) return std::uint32_t(Stop::unsupported_numeric);
    if (destination) vectors->values[destination-1]=result.value;
    vectors->mac=result.mac; vectors->status=result.status;
    return std::uint32_t(Stop::budget);
}
}
extern "C" std::uint32_t nt_ee_vector_add(nt::ee::Memory* memory,std::uint32_t destination,std::uint32_t left,
                                         std::uint32_t right,std::uint32_t mask,std::uint32_t subtract,std::uint32_t broadcast) {
    if (subtract>1) return std::uint32_t(nt::ee::Stop::invalid_state);
    return vector_arithmetic(memory,destination,left,right,mask,broadcast,
        [subtract](auto a,auto b) { return nt::numeric::add_subtract_finite(a,b,subtract!=0); });
}
extern "C" std::uint32_t nt_ee_vector_multiply(nt::ee::Memory* memory,std::uint32_t destination,std::uint32_t left,
                                              std::uint32_t right,std::uint32_t mask,std::uint32_t broadcast) {
    return vector_arithmetic(memory,destination,left,right,mask,broadcast,nt::numeric::multiply_finite);
}
