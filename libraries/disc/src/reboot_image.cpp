#include <nt/reboot_image.hpp>
#include <algorithm>
#include <set>
#include <stdexcept>

namespace nt::disc {
std::vector<RebootFile> parse_reboot_image(ByteView image) {
    const auto fail = [](const char* reason) { throw std::runtime_error(std::string("IOP image: ")+reason); };
    const auto name_at = [&](std::size_t at) {
        require_range(image,at,16);
        const auto field=image.subspan(at,10);
        const auto end=std::find(field.begin(),field.end(),0);
        if (std::any_of(field.begin(),end,[](auto c) { return c<33 || c>126 || c=='/' || c=='\\'; }))
            fail("invalid directory name");
        if (std::any_of(end,field.end(),[](auto c) { return c!=0; })) fail("invalid name padding");
        return std::string(field.begin(),end);
    };
    if (name_at(0)!="RESET" || le32(image,12)!=0) fail("expected a disc module archive with empty RESET");
    if (name_at(16)!="ROMDIR" || name_at(32)!="EXTINFO") fail("missing directory or extended information");
    const std::size_t directory_size=le32(image,28), info_size=le32(image,44);
    if (directory_size<64 || directory_size%16 || directory_size>4096*16) fail("invalid directory size");
    require_range(image,0,directory_size);
    require_range(image,directory_size,info_size);
    std::vector<RebootFile> files;
    std::set<std::string> names;
    std::size_t offset=0, info_offset=0;
    for (std::size_t at=0;at<directory_size;at+=16) {
        auto name=name_at(at);
        if (name.empty()) {
            if (at+16!=directory_size || std::any_of(image.begin()+at,image.begin()+at+16,[](auto c) { return c!=0; }))
                fail("invalid directory terminator");
            if (info_offset!=info_size) fail("unclaimed extended information");
            return files;
        }
        if (!names.insert(name).second) fail("duplicate directory name");
        const std::size_t size=le32(image,at+12), extra=le16(image,at+10);
        require_range(image,offset,size);
        if (extra%4 || info_offset>info_size || extra>info_size-info_offset) fail("invalid extended information extent");
        const auto info=image.subspan(directory_size+info_offset,extra);
        for (std::size_t field=0;field<info.size();) {
            require_range(info,field,4);
            const std::size_t length=4+info[field+2];
            if (length%4) fail("unaligned extended information field");
            require_range(info,field,length);
            field+=length;
        }
        files.push_back({std::move(name),offset,size,directory_size+info_offset,extra});
        offset+=(size+15)&~std::size_t(15);
        info_offset+=extra;
    }
    fail("missing directory terminator");
    return {};
}
}
