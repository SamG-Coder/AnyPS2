#include <nt/iop_resources.hpp>
#include <nt/reboot_image.hpp>
#include <algorithm>
#include <stdexcept>
#include <string_view>

namespace nt::iop {
Resources::Resources(std::span<const ResourceFile> files) {
    constexpr std::size_t capacity=0x40000;
    if (files.size()>4092) throw std::runtime_error("IOP resources: too many files");
    const auto directory_size=(files.size()+4)*16;
    auto total=directory_size;
    for (const auto& file : files) {
        if (file.name.empty() || file.name.size()>10 || file.data.size()>capacity)
            throw std::runtime_error("IOP resources: invalid file name or size");
        const auto size=(file.data.size()+15)&~std::size_t(15);
        if (size>capacity-total) throw std::runtime_error("IOP resources: directory exceeds address window");
        total+=size;
    }
    image_.resize(total);
    const auto record=[&](std::size_t index,std::string_view name,std::uint32_t size) {
        const auto at=index*16;
        std::copy(name.begin(),name.end(),image_.begin()+at);
        for (unsigned i=0;i<4;++i) image_[at+12+i]=static_cast<std::uint8_t>(size>>(8*i));
    };
    record(0,"RESET",0); record(1,"ROMDIR",static_cast<std::uint32_t>(directory_size)); record(2,"EXTINFO",0);
    auto at=directory_size;
    for (std::size_t index=0;index<files.size();++index) {
        const auto& file=files[index];
        record(index+3,file.name,static_cast<std::uint32_t>(file.data.size()));
        std::copy(file.data.begin(),file.data.end(),image_.begin()+at);
        at+=(file.data.size()+15)&~std::size_t(15);
    }
    // Share validation of names, duplicates, extents and the directory terminator.
    (void)disc::parse_reboot_image(image_);
}
std::optional<std::uint32_t> Resources::read(std::uint32_t address,unsigned bytes) const {
    constexpr std::uint32_t base=0x1fc00000;
    if ((bytes!=1 && bytes!=2 && bytes!=4) || address%bytes || address<base) return {};
    const auto offset=address-base;
    if (offset>image_.size() || bytes>image_.size()-offset) return {};
    std::uint32_t value=0;
    for (unsigned i=0;i<bytes;++i) value|=std::uint32_t(image_[offset+i])<<(8*i);
    return value;
}
}
