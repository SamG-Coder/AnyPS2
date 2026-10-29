#include <nt/iop_module.hpp>
#include <algorithm>
#include <optional>
#include <stdexcept>

namespace nt::elf {
namespace {
[[noreturn]] void invalid(const char* detail) {
    throw std::runtime_error(std::string("IOP module: ")+detail);
}
void put32(Bytes& bytes, std::size_t at, std::uint32_t value) {
    for (unsigned i=0;i<4;++i) bytes[at+i]=static_cast<std::uint8_t>(value>>(8*i));
}
}
IopModule load_iop_module(ByteView file, std::uint32_t base) {
    require_range(file,0,52);
    constexpr std::uint8_t identity[]{0x7f,'E','L','F',1,1,1};
    if (!std::equal(std::begin(identity),std::end(identity),file.begin()) ||
        le16(file,18)!=8 || le32(file,20)!=1 || le16(file,40)!=52)
        invalid("expected little-endian MIPS ELF32");
    if (le16(file,16)!=0xff80) invalid("unsupported ELF type (expected version-one relocatable IRX)");
    constexpr std::uint32_t ram_size=2*1024*1024;
    if (base%16 || base>=ram_size) invalid("invalid load base");
    const auto ph=le32(file,28), sh=le32(file,32);
    const auto ph_stride=le16(file,42), ph_count=le16(file,44);
    const auto sh_stride=le16(file,46), sh_count=le16(file,48);
    if (ph_stride<32 || !ph_count || sh_stride<40 || !sh_count) invalid("invalid header tables");
    require_range(file,ph,std::size_t(ph_stride)*ph_count);
    require_range(file,sh,std::size_t(sh_stride)*sh_count);
    std::optional<std::size_t> load, metadata;
    for (unsigned i=0;i<ph_count;++i) {
        const std::size_t at=ph+std::size_t(i)*ph_stride;
        if (le32(file,at)==0x70000080) {
            if (metadata) invalid("multiple module metadata headers");
            metadata=at;
        }
        if (le32(file,at)!=1) continue;
        if (load) invalid("multiple load segments are unsupported");
        load=at;
    }
    if (!load) invalid("missing load segment");
    const auto at=*load;
    const auto offset=le32(file,at+4), size=le32(file,at+16), memory_size=le32(file,at+20);
    const auto alignment=le32(file,at+28), entry=le32(file,24);
    if (le32(file,at+8)!=0 || size>memory_size || memory_size>ram_size-base)
        invalid("load segment exceeds IOP allocation");
    if (alignment>1 && ((alignment&(alignment-1)) || base%alignment)) invalid("invalid segment alignment");
    if (!(le32(file,at+24)&1) || entry%4 || entry>size || size-entry<4)
        invalid("entry is not file-backed executable memory");
    require_range(file,offset,size);
    IopModule result{base,base+entry,Bytes(memory_size,0),size,{}};
    std::copy_n(file.begin()+offset,size,result.memory.begin());
    if (metadata) {
        const auto start=le32(file,*metadata+4), length=le32(file,*metadata+16);
        require_range(file,start,length);
        const auto data=file.subspan(start,length);
        require_range(data,0,27);
        const auto name=data.subspan(26);
        const auto end=std::find(name.begin(),name.end(),0);
        if (end==name.end() || end-name.begin()>255) invalid("unterminated module name");
        IopModuleInfo info{std::string(name.begin(),end),le16(data,24),le32(data,8),
                           le32(data,12),le32(data,16),le32(data,20)};
        if (std::uint64_t(info.text_size)+info.data_size>size ||
            std::uint64_t(info.text_size)+info.data_size+info.bss_size>memory_size)
            invalid("module metadata exceeds load segment");
        if (info.global_pointer>=ram_size-base) invalid("module global pointer exceeds IOP RAM");
        info.global_pointer+=base;
        result.info=std::move(info);
    }
    for (unsigned i=0;i<sh_count;++i) {
        const std::size_t section=sh+std::size_t(i)*sh_stride;
        const auto type=le32(file,section+4);
        if (type==4) invalid("RELA relocations are unsupported");
        if (type!=9) continue;
        const auto start=le32(file,section+16), bytes=le32(file,section+20);
        if (le32(file,section+36)!=8 || bytes%8) invalid("invalid relocation table");
        require_range(file,start,bytes);
        std::optional<std::uint32_t> high;
        for (std::size_t record=start;record<std::size_t(start)+bytes;record+=8) {
            const auto target=le32(file,record), kind=le32(file,record+4)&255;
            if (!kind) continue;
            if (target%4 || target>size || size-target<4) invalid("relocation target outside file-backed segment");
            const auto word=le32(result.memory,target);
            switch (kind) {
            case 2: put32(result.memory,target,word+base); break;
            case 4:
                put32(result.memory,target,(word&0xfc000000U)|(((word&0x03ffffffU)+(base>>2))&0x03ffffffU));
                break;
            case 5:
                if (high) invalid("unpaired HI16 relocation");
                high=target;
                break;
            case 6: {
                if (!high) invalid("LO16 relocation without HI16");
                const auto upper=le32(result.memory,*high);
                const std::uint32_t low=(word&0x8000) ? (word|0xffff0000U) : (word&0xffffU);
                const auto address=(upper<<16)+low+base;
                put32(result.memory,*high,(upper&0xffff0000U)|(((address+0x8000U)>>16)&0xffffU));
                put32(result.memory,target,(word&0xffff0000U)|(address&0xffffU));
                high.reset();
                break;
            }
            default: invalid("unsupported relocation type");
            }
        }
        if (high) invalid("unterminated HI16 relocation");
    }
    return result;
}
}
