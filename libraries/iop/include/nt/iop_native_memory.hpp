#pragma once
#include <nt/iop_native.hpp>
#include <cstring>

namespace nt::iop {
enum class NativeMemoryOperation { fill, copy, move };
template<class IsCode>
NativeResult native_memory(Cpu& cpu,std::span<std::uint8_t> ram,IsCode is_code,NativeMemoryOperation operation) {
    if (cpu.delay_slot) return {NativeStop::branch_in_delay,cpu.pc};
    const auto reg=[&](unsigned index) { return cpu.load_register==index ? cpu.load_value : cpu.registers[index]; };
    const auto destination=reg(4), value=reg(5), count=reg(6), target=reg(31);
    if (count) {
        const auto offset=ram_offset(destination,1,ram.size());
        if (!offset || count>ram.size()-*offset || count>0x200000-*offset)
            return {NativeStop::address_fault,cpu.pc,destination};
        std::optional<std::uint32_t> source;
        if (operation!=NativeMemoryOperation::fill) {
            source=ram_offset(value,1,ram.size());
            if (!source || count>ram.size()-*source || count>0x200000-*source)
                return {NativeStop::address_fault,cpu.pc,value};
            if (operation==NativeMemoryOperation::copy && *offset<*source+count && *source<*offset+count)
                return {NativeStop::address_fault,cpu.pc,destination};
        }
        for (auto word=*offset&~3U;word<*offset+count;word+=4)
            if (is_code(word)) return {NativeStop::code_write,cpu.pc,destination};
        if (operation==NativeMemoryOperation::fill) std::memset(ram.data()+*offset,static_cast<unsigned char>(value),count);
        else if (operation==NativeMemoryOperation::copy) std::memcpy(ram.data()+*offset,ram.data()+*source,count);
        else std::memmove(ram.data()+*offset,ram.data()+*source,count);
    }
    if (cpu.load_register && cpu.load_register!=2) cpu.registers[cpu.load_register]=cpu.load_value;
    cpu.load_register=0; cpu.registers[2]=destination; cpu.registers[0]=0;
    cpu.pc=target; ++cpu.executed;
    return {NativeStop::budget,cpu.pc};
}
}
