#pragma once
#include <nt/ee_handlers.hpp>
#include <vector>
#include <string>
#include <cstring>
namespace nt::ee {
// Host-owned copies survive clearing the previous image's argument storage.
class LaunchArguments {
public:
    Stop capture(Memory& memory,std::uint32_t count,std::uint32_t argv) {
        if (count>15) return Stop::unsupported_system;
        std::vector<std::string> next;
        std::size_t total=0;
        const auto table=runtime::default_ram_address(argv);
        if (count && !range(memory,table,count*4,4)) { memory.fault_address=argv; return Stop::memory_fault; }
        for (unsigned n=0;n<count;++n) {
            std::uint32_t raw=0; std::memcpy(&raw,memory.data+table+n*4,4);
            auto address=runtime::default_ram_address(raw);
            std::string text;
            while (true) {
                if (total==256) return Stop::unsupported_system;
                if (!raw || !range(memory,address,1,1)) { memory.fault_address=raw; return Stop::memory_fault; }
                ++total; const auto byte=memory.data[address++];
                if (!byte) break;
                text.push_back(static_cast<char>(byte));
            }
            next.push_back(std::move(text));
        }
        arguments_=std::move(next); return Stop::budget;
    }
    Stop write(Memory& memory,std::uint32_t raw,CodeOverlap protected_code) const {
        std::size_t bytes=4+(arguments_.size()+1)*4;
        for (const auto& argument:arguments_) bytes+=argument.size()+1;
        const auto address=runtime::default_ram_address(raw);
        if (!range(memory,address,bytes,4) || std::uint64_t(raw)+bytes>0x100000000ULL) {
            memory.fault_address=raw; return Stop::memory_fault;
        }
        if (!protected_code) return Stop::invalid_state;
        if (protected_code(address,static_cast<std::uint32_t>(bytes))) { memory.fault_address=raw; return Stop::code_write; }
        std::vector<std::uint8_t> data(bytes,0);
        const auto word=[&](std::size_t at,std::uint32_t value) { std::memcpy(data.data()+at,&value,4); };
        word(0,static_cast<std::uint32_t>(arguments_.size()));
        std::size_t cursor=4+(arguments_.size()+1)*4;
        for (std::size_t n=0;n<arguments_.size();++n) {
            word(4+n*4,raw+static_cast<std::uint32_t>(cursor));
            std::memcpy(data.data()+cursor,arguments_[n].c_str(),arguments_[n].size()+1);
            cursor+=arguments_[n].size()+1;
        }
        std::memcpy(memory.data+address,data.data(),data.size()); return Stop::budget;
    }
private:
    static bool range(const Memory& memory,std::uint32_t address,std::size_t bytes,unsigned alignment) {
        return memory.data && address%alignment==0 && std::uint64_t(address)+bytes<=memory.size &&
            std::uint64_t(address)+bytes<=0x2000000U;
    }
    std::vector<std::string> arguments_;
};
}
