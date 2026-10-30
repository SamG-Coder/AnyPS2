#include <nt/ee_native.hpp>
#include <nt/ee_interrupts.hpp>
#include <nt/ee_completions.hpp>
namespace {
void publish(nt::ee::Memory& memory,nt::ee::Stop status,std::uint32_t address) {
    if (memory.io.poll) memory.io.poll(memory.io.context);
    if (memory.interrupts) memory.interrupts->publish(memory);
    if (memory.completions) memory.completions->publish(memory);
    if (status!=nt::ee::Stop::budget) memory.fault_address=address;
}
}
extern "C" std::uint64_t nt_ee_io_read32(nt::ee::Memory* memory,std::uint32_t address) {
    const auto result=memory->io.read32 ? memory->io.read32(memory->io.context,address) :
        nt::ee::IoRead{nt::ee::Stop::memory_fault};
    publish(*memory,result.status,address);
    return (std::uint64_t(result.status)<<32)|result.value;
}
extern "C" std::uint32_t nt_ee_io_write32(nt::ee::Memory* memory,std::uint32_t address,std::uint32_t value) {
    const auto result=memory->io.write32 ? memory->io.write32(memory->io.context,address,value) : nt::ee::Stop::memory_fault;
    publish(*memory,result,address);
    return static_cast<std::uint32_t>(result);
}

extern "C" std::uint32_t nt_ee_io_read64(nt::runtime::State* state,nt::ee::Memory* memory,std::uint32_t address,std::uint32_t reg) {
    const auto result=memory->io.read64 ? memory->io.read64(memory->io.context,address) : nt::ee::IoRead64{nt::ee::Stop::memory_fault};
    publish(*memory,result.status,address);
    if (result.status==nt::ee::Stop::budget && reg && reg<32) state->set(reg,result.value);
    return static_cast<std::uint32_t>(result.status);
}
extern "C" std::uint32_t nt_ee_io_write64(nt::ee::Memory* memory,std::uint32_t address,std::uint64_t value) {
    const auto result=memory->io.write64 ? memory->io.write64(memory->io.context,address,value) : nt::ee::Stop::memory_fault;
    publish(*memory,result,address);
    return static_cast<std::uint32_t>(result);
}

extern "C" std::uint64_t nt_ee_address_space(nt::ee::Memory* memory,std::uint32_t operation,std::uint32_t parameter,std::uint32_t value) {
    using nt::ee::AddressSpaceOperation;
    if (operation>static_cast<std::uint32_t>(AddressSpaceOperation::probe))
        return std::uint64_t(nt::ee::Stop::invalid_state)<<32;
    const auto result=memory->address_space.request ?
        memory->address_space.request(memory->address_space.context,static_cast<AddressSpaceOperation>(operation),parameter,value) :
        nt::ee::IoRead{nt::ee::Stop::unsupported_system};
    return (std::uint64_t(result.status)<<32)|result.value;
}
