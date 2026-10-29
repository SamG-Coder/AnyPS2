#include <nt/iop_startup.hpp>
#include <algorithm>
#include <stdexcept>

namespace nt::iop {
namespace {
void validate_call(std::span<std::uint8_t> ram,const elf::IopModule& module,std::uint32_t entry,
                   std::uint32_t stack_begin,std::uint32_t stack_end) {
    constexpr std::uint32_t ram_size=2*1024*1024;
    const std::uint64_t module_end=std::uint64_t(module.base)+module.memory.size();
    if (ram.size()!=ram_size || module.base%16 || module_end>ram_size ||
        module.initialized_size>module.memory.size() || entry%4 || entry<module.base ||
        std::uint64_t(entry)+4>std::uint64_t(module.base)+module.initialized_size)
        throw std::runtime_error("IOP startup: invalid module allocation or entry");
    if (stack_begin%16 || stack_end%16 || stack_end>ram_size || stack_begin>=stack_end ||
        (stack_begin<module_end && stack_end>module.base))
        throw std::runtime_error("IOP startup: invalid or overlapping stack allocation");
    if (module.info && module.info->global_pointer>=ram_size)
        throw std::runtime_error("IOP startup: global pointer exceeds RAM");
}
}
Startup::Startup(std::span<std::uint8_t> ram,const elf::IopModule& owner,std::uint32_t entry,
                 std::uint32_t stack_begin,std::uint32_t stack_end,EntryRegisters arguments,Devices* devices,HostCalls* calls)
    : ram_(ram), devices_(devices), calls_(calls) {
    validate_call(ram,owner,entry,stack_begin,stack_end);
    cpu_.pc=entry;
    cpu_.registers[28]=owner.info ? owner.info->global_pointer : 0;
    cpu_.registers[29]=stack_end-16;
    cpu_.registers[31]=return_address;
    std::copy(arguments.arguments.begin(),arguments.arguments.end(),cpu_.registers.begin()+4);
    std::fill(ram.begin()+stack_end-16,ram.begin()+stack_end,std::uint8_t{0});
}
Startup::Startup(std::span<std::uint8_t> ram,elf::IopModule module,const Libraries& libraries,
                 std::uint32_t stack_begin,std::uint32_t stack_end,EntryRegisters arguments,Devices* devices,HostCalls* calls)
    : Startup(ram,std::move(module),libraries,stack_begin,stack_end,std::span<const std::string>{},devices,calls) {
    std::copy(arguments.arguments.begin(),arguments.arguments.end(),cpu_.registers.begin()+4);
}
Startup::Startup(std::span<std::uint8_t> ram,elf::IopModule module,const Libraries& libraries,
                 std::uint32_t stack_begin,std::uint32_t stack_end,std::span<const std::string> arguments,Devices* devices,HostCalls* calls)
    : ram_(ram), devices_(devices), calls_(calls) {
    validate_call(ram,module,module.entry,stack_begin,stack_end);
    if (arguments.size()>64) throw std::runtime_error("IOP startup: argument count exceeds limit");
    std::size_t bytes=0;
    for (const auto& argument : arguments) {
        if (argument.find('\0')!=std::string::npos || argument.size()>4095)
            throw std::runtime_error("IOP startup: invalid argument string");
        bytes+=argument.size()+1;
    }
    const auto strings_size=(bytes+15)&~std::size_t(15);
    const auto pointers_size=((arguments.size()+1)*4+15)&~std::size_t(15);
    const auto total=strings_size+pointers_size+16;
    if (total>stack_end-stack_begin) throw std::runtime_error("IOP startup: arguments exceed stack allocation");
    const auto sp=stack_end-static_cast<std::uint32_t>(total), argv=sp+16;
    const auto strings=argv+static_cast<std::uint32_t>(pointers_size);
    Bytes frame(total,0);
    std::size_t position=0;
    for (std::size_t i=0;i<arguments.size();++i) {
        const auto pointer=strings+static_cast<std::uint32_t>(position);
        for (unsigned byte=0;byte<4;++byte) frame[16+i*4+byte]=static_cast<std::uint8_t>(pointer>>(byte*8));
        std::copy(arguments[i].begin(),arguments[i].end(),frame.begin()+16+pointers_size+position);
        position+=arguments[i].size()+1;
    }
    libraries.bind(module);
    cpu_.pc=module.entry;
    cpu_.registers[4]=static_cast<std::uint32_t>(arguments.size());
    cpu_.registers[5]=argv;
    cpu_.registers[28]=module.info ? module.info->global_pointer : 0;
    cpu_.registers[29]=sp;
    cpu_.registers[31]=return_address;
    // No caller RAM changes until all validation and import resolution succeeds.
    std::copy(module.memory.begin(),module.memory.end(),ram.begin()+module.base);
    std::copy(frame.begin(),frame.end(),ram.begin()+sp);
}
StartupResult Startup::resume(std::uint64_t budget) {
    if (terminal_) return *terminal_;
    while (budget--) {
        const auto result=run(cpu_,ram_,1,devices_,calls_);
        if (result.reason!=CpuStop::budget) {
            terminal_=StartupResult{StartupStop::fault,0,result};
            return *terminal_;
        }
        if (cpu_.pc==return_address && !cpu_.delay_slot) {
            terminal_=StartupResult{StartupStop::returned,cpu_.registers[2],result};
            return *terminal_;
        }
    }
    return {StartupStop::budget,0,{CpuStop::budget,cpu_.pc,0}};
}
}
