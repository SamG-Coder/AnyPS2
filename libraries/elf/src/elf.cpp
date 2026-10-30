#include <nt/elf.hpp>
#include <algorithm>
#include <stdexcept>

namespace nt::elf {
namespace {
void lifecycle_entries(ByteView bytes,Executable& executable) {
    const auto table=le32(bytes,32);
    const auto stride=le16(bytes,46),count=le16(bytes,48),names_index=le16(bytes,50);
    if (!table && !count) return;
    if (!table || !count || count>4096 || stride<40 || names_index>=count)
        throw std::runtime_error("Invalid ELF section headers");
    require_range(bytes,table,std::size_t(stride)*count);
    ByteView names;
    if (names_index) {
        const auto at=std::size_t(table)+names_index*stride;
        if (le32(bytes,at+4)!=3) throw std::runtime_error("Invalid ELF section name table");
        const auto offset=le32(bytes,at+16),size=le32(bytes,at+20);
        require_range(bytes,offset,size); names=bytes.subspan(offset,size);
    }
    for (unsigned index=0;index<count;++index) {
        const auto at=std::size_t(table)+index*stride;
        const auto type=le32(bytes,at+4),flags=le32(bytes,at+8);
        if (!(flags&2)) continue; // Only allocated tables belong to the program.
        bool legacy=false;
        if (type==1 && !names.empty()) {
            const auto name=le32(bytes,at);
            if (name>=names.size()) throw std::runtime_error("Invalid ELF section name");
            const auto end=std::find(names.begin()+name,names.end(),0);
            if (end==names.end()) throw std::runtime_error("Unterminated ELF section name");
            const std::string text(names.begin()+name,end);
            legacy=text==".ctors" || text==".dtors";
        }
        const bool array=type==14 || type==15 || type==16; // INIT, FINI, PREINIT_ARRAY.
        if (!legacy && !array) continue;
        const auto address=le32(bytes,at+12),offset=le32(bytes,at+16),size=le32(bytes,at+20),entry_size=le32(bytes,at+36);
        if (address%4 || size%4 || (entry_size!=4 && (!legacy || entry_size)))
            throw std::runtime_error("Invalid ELF lifecycle table layout");
        require_range(bytes,offset,size);
        const auto mapped=std::any_of(executable.segments.begin(),executable.segments.end(),[&](const auto& segment) {
            return address>=segment.address && std::uint64_t(address)+size<=std::uint64_t(segment.address)+segment.file_size &&
                std::uint64_t(segment.offset)+address-segment.address==offset;
        });
        if (!mapped) throw std::runtime_error("ELF lifecycle table is not mapped file data");
        for (std::uint32_t item=0;item<size;item+=4) {
            auto target=le32(bytes,std::size_t(offset)+item);
            if (!target || (legacy && target==0xffffffffU)) continue;
            if (legacy && !item && size>=8 && target==size/4-2 && !le32(bytes,std::size_t(offset)+size-4)) continue;
            if (target>=0x80000000U && target<0xc0000000U) target&=0x1fffffffU;
            const auto executable_target=std::any_of(executable.segments.begin(),executable.segments.end(),[&](const auto& segment) {
                return (segment.flags&1) && target>=segment.address && std::uint64_t(target)+4<=std::uint64_t(segment.address)+segment.file_size;
            });
            if (target%4 || !executable_target) throw std::runtime_error("Invalid ELF lifecycle code pointer");
            executable.lifecycle_entries.push_back(target);
        }
    }
    auto& entries=executable.lifecycle_entries;
    std::sort(entries.begin(),entries.end()); entries.erase(std::unique(entries.begin(),entries.end()),entries.end());
}
}
Executable parse(ByteView b) {
    require_range(b, 0, 52);
    constexpr std::uint8_t magic[] = {0x7f, 'E', 'L', 'F', 1, 1, 1};
    if (!std::equal(std::begin(magic), std::end(magic), b.begin()))
        throw std::runtime_error("Expected little-endian ELF32");
    if (le16(b, 16) != 2 || le16(b, 18) != 8 || le32(b, 20) != 1 || le16(b, 40) != 52)
        throw std::runtime_error("Expected MIPS ET_EXEC ELF");
    Executable result{le32(b, 24), le32(b, 36), {}};
    const auto table = le32(b, 28);
    const auto stride = le16(b, 42), count = le16(b, 44);
    if (stride < 32 || count == 0) throw std::runtime_error("Invalid ELF program headers");
    require_range(b, table, std::size_t(stride) * count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto p = table + i * stride;
        if (le32(b, p) != 1) continue;
        Segment s{le32(b, p + 4), le32(b, p + 8), le32(b, p + 16),
                  le32(b, p + 20), le32(b, p + 24), le32(b, p + 28)};
        require_range(b, s.offset, s.file_size);
        if (s.file_size > s.memory_size || std::uint64_t(s.address) + s.memory_size > 0x100000000ULL)
            throw std::runtime_error("Invalid ELF segment extent");
        if (s.alignment > 1 && ((s.alignment & (s.alignment - 1)) ||
            (s.address % s.alignment != s.offset % s.alignment)))
            throw std::runtime_error("Invalid ELF segment alignment");
        result.segments.push_back(s);
    }
    const auto contains_entry = [&](const Segment& s) {
        return (s.flags & 1) && result.entry >= s.address &&
               std::uint64_t(result.entry) + 4 <= std::uint64_t(s.address) + s.file_size;
    };
    if (result.entry % 4 || !std::any_of(result.segments.begin(), result.segments.end(), contains_entry))
        throw std::runtime_error("Entry is not aligned, file-backed executable memory");
    lifecycle_entries(b,result);
    return result;
}
void describe(std::ostream& out, const Executable& executable) {
    out << "MIPS ELF32 entry " << hex32(executable.entry) << " flags " << hex32(executable.flags) << '\n';
    for (const auto& s : executable.segments)
        out << "  LOAD " << hex32(s.address) << " file=" << s.file_size << " memory=" << s.memory_size
            << " flags=" << s.flags << " offset=" << s.offset << '\n';
    out << "Import metadata only; native translation has not been performed.\n";
}
}
