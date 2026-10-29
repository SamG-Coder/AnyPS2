#include <nt/iop_native_library.hpp>
#include <nt/iop_memory.hpp>
#include <algorithm>
#include <stdexcept>

namespace nt::iop {
NativeLibrary::NativeLibrary(std::string_view name,std::uint16_t version,unsigned count,std::uint32_t base)
    : count_(count), code_offset_((24+count*4+15)&~15U) {
    if (!count || count>256 || name.empty() || name.size()>8 ||
        std::any_of(name.begin(),name.end(),[](unsigned char c) { return c<33 || c>126; }) ||
        !base || base%16 || base>2*1024*1024-allocation_size(count))
        throw std::runtime_error("IOP native library: invalid descriptor or allocation");
    const auto size=allocation_size(count);
    module_={base,base+code_offset_,Bytes(size,0),size,{}};
    const auto word=[&](unsigned at,std::uint32_t value) {
        for (unsigned i=0;i<4;++i) module_.memory[at+i]=static_cast<std::uint8_t>(value>>(8*i));
    };
    word(0,0x41c00000); word(8,version);
    std::copy(name.begin(),name.end(),module_.memory.begin()+12);
    for (unsigned i=0;i<count;++i) {
        const auto offset=code_offset_+i*12;
        word(20+i*4,base+offset);
        word(offset,12); word(offset+4,0x03e00008); // Host bridge; jr ra; nop.
    }
}
void NativeLibrary::install(std::span<std::uint8_t> ram,Libraries& libraries) {
    if (installed_ || std::uint64_t(module_.base)+module_.memory.size()>ram.size())
        throw std::runtime_error("IOP native library: invalid or repeated installation");
    libraries.register_exports(module_,module_.base);
    std::copy(module_.memory.begin(),module_.memory.end(),ram.begin()+module_.base);
    installed_=true;
}
std::optional<unsigned> NativeLibrary::ordinal(std::uint32_t pc,std::size_t ram_size) const {
    const auto address=ram_offset(pc,4,ram_size);
    if (!installed_ || !address || *address<module_.base+code_offset_) return {};
    const auto offset=*address-module_.base-code_offset_;
    if (offset%12 || offset/12>=count_) return {};
    return offset/12;
}
}
