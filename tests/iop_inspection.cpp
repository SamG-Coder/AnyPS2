#include <nt/inventory.hpp>
#include <iostream>
#include <stdexcept>

int main() {
    try {
        nt::elf::IopModule module{0x1000,0x1000,nt::Bytes(0x400),0x400,{}};
        const auto word=[&](unsigned offset,std::uint32_t value) {
            for (unsigned byte=0;byte<4;++byte) module.memory[offset+byte]=static_cast<std::uint8_t>(value>>(byte*8));
        };
        const std::uint32_t instructions[]{0x27bdffe0,0x3c080000,0x35081100,0xafa80010,
            0xafa80018,0x27a70018,0x0c0004c5,0x24040007,0x03e00008,0,0xffffffff};
        for (unsigned index=0;index<std::size(instructions);++index) word(index*4,instructions[index]);
        word(0x100,0x03e00008); word(0x300,0x41e00000); word(0x308,0x102);
        const std::string name="usbd"; std::copy(name.begin(),name.end(),module.memory.begin()+0x30c);
        word(0x314,0x03e00008); word(0x318,0x2400000b);
        const auto report=nt::iop_codegen::ir::inspect_neighborhood(module,0x1010,0x40);
        if (report.calls.size()!=1 || !report.calls[0].import || report.calls[0].import->library!="usbd" ||
            report.calls[0].import->ordinal!=11 || report.calls[0].arguments[0]!=std::set<std::uint32_t>{7} ||
            report.calls[0].arguments[4]!=std::set<std::uint32_t>{0x1100} ||
            !report.calls[0].code_candidates.contains(0x1100) || !report.unknown.contains(0x1028) ||
            !report.calls[0].stack_arguments.contains(3) || report.calls[0].stack_arguments.at(3)[1]!=0x11 ||
            report.calls[0].stack_arguments.at(3)[4].has_value())
            throw std::runtime_error("Neighborhood lost imports, delay-slot arguments, stack callbacks or unsupported words");
        const auto text=nt::inventory::format_neighborhood(report);
        if (text.find("sceUsbdTransferPipe")==std::string::npos || text.find("arg4=0x00001100")==std::string::npos)
            throw std::runtime_error("Neighborhood did not reuse public call names or expose the callback argument");
        for (const auto anchor:{0xffcU,0x1400U,0x1011U}) {
            bool rejected=false;
            try { nt::iop_codegen::ir::inspect_neighborhood(module,anchor,0x40); } catch (const std::invalid_argument&) { rejected=true; }
            if (!rejected) throw std::runtime_error("Invalid neighborhood anchor accepted");
        }
        std::cout<<"Offline IOP neighborhood inspection passed\n";
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
