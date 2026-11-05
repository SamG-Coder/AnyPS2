#include <nt/elf.hpp>
#include <algorithm>
#include <array>
#include <optional>
#include <stdexcept>

namespace nt::elf {
namespace {
void import_note(ByteView bytes,Executable& executable) {
    std::size_t at=0;
    while (at<bytes.size()) {
        require_range(bytes,at,12);
        const auto name_size=le32(bytes,at),data_size=le32(bytes,at+4),type=le32(bytes,at+8);
        const auto name_at=std::uint64_t(at)+12;
        const auto data_at=name_at+((std::uint64_t(name_size)+3)&~3ULL);
        const auto next=data_at+((std::uint64_t(data_size)+3)&~3ULL);
        if (next>bytes.size()) throw std::runtime_error("Invalid ELF note extent");
        constexpr std::array<std::uint8_t,7> name{'A','n','y','P','S','2',0};
        if (name_size==name.size() && std::equal(name.begin(),name.end(),bytes.begin()+name_at) && type==1) {
            if (data_size!=4 || le32(bytes,static_cast<std::size_t>(data_at))!=1 || executable.inferred_permissions)
                throw std::runtime_error("Invalid AnyPS2 import permissions note");
            executable.inferred_permissions=true;
        }
        at=static_cast<std::size_t>(next);
    }
}
void read_sections(ByteView bytes,Executable& executable) {
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
    const auto section_name_at=[&](std::size_t at) {
        if (names.empty()) return std::string{};
        const auto name=le32(bytes,at);
        if (name>=names.size()) throw std::runtime_error("Invalid ELF section name");
        const auto end=std::find(names.begin()+name,names.end(),0);
        if (end==names.end()) throw std::runtime_error("Unterminated ELF section name");
        return std::string(names.begin()+name,end);
    };
    for (unsigned index=0;index<count;++index) {
        const auto at=std::size_t(table)+index*stride;
        const auto type=le32(bytes,at+4),flags=le32(bytes,at+8);
        if (!(flags&2)) continue; // Only allocated tables belong to the program.
        const auto section_address=le32(bytes,at+12),section_offset=le32(bytes,at+16),section_size=le32(bytes,at+20);
        const auto section_name=section_name_at(at);
        const bool vector_code=type==1 && section_name==".vutext";
        if (section_size && (flags&4) && !vector_code) {
            if (type!=1 || section_address%4 || section_size%4)
                throw std::runtime_error("Invalid ELF EE instruction section layout");
            require_range(bytes,section_offset,section_size);
            const auto mapped=std::any_of(executable.segments.begin(),executable.segments.end(),[&](const auto& segment) {
                return (segment.flags&1) && section_address>=segment.address &&
                    std::uint64_t(section_address)+section_size<=std::uint64_t(segment.address)+segment.file_size &&
                    std::uint64_t(segment.offset)+section_address-segment.address==section_offset;
            });
            if (!mapped) throw std::runtime_error("ELF EE instruction section is not mapped executable file data");
            executable.code_sections.push_back({section_address,section_size});
            if (flags&1) executable.writable_code=true;
        }
        if (section_size && (!(flags&4) || vector_code)) {
            const auto end=std::uint64_t(section_address)+section_size;
            const auto mapped=std::any_of(executable.segments.begin(),executable.segments.end(),[&](const auto& segment) {
                return section_address>=segment.address && end<=std::uint64_t(segment.address)+(type==8 ? segment.memory_size : segment.file_size) &&
                    (type==8 || std::uint64_t(segment.offset)+section_address-segment.address==section_offset);
            });
            if (end>0x100000000ULL || !mapped) throw std::runtime_error("ELF allocated data section is not mapped");
            if (type!=8) require_range(bytes,section_offset,section_size);
            executable.non_executable_sections.push_back({section_address,section_size});
            if (vector_code) executable.vector_sections.push_back({section_address,section_size});
        }
        const bool legacy=type==1 && (section_name==".ctors" || section_name==".dtors");
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
    bool overlays_seen=false;
    for (unsigned index=0;index<count;++index) {
        const auto at=std::size_t(table)+index*stride;
        if (le32(bytes,at+4)!=0x7ffff420U) continue;
        const auto offset=le32(bytes,at+16),size=le32(bytes,at+20),link=le32(bytes,at+24);
        if (overlays_seen || le32(bytes,at+36)!=12 || size%12 || size/12>4096 || link>=count)
            throw std::runtime_error("Invalid ELF DVP overlay table");
        overlays_seen=true; require_range(bytes,offset,size);
        const auto strings=std::size_t(table)+link*stride;
        if (le32(bytes,strings+4)!=3) throw std::runtime_error("Invalid ELF DVP overlay strings");
        const auto string_offset=le32(bytes,strings+16),string_size=le32(bytes,strings+20);
        require_range(bytes,string_offset,string_size);
        const auto text=bytes.subspan(string_offset,string_size);
        for (unsigned item=0;item<size;item+=12) {
            const auto row=std::size_t(offset)+item;
            const auto name_offset=le32(bytes,row),source=le32(bytes,row+4),destination=le32(bytes,row+8);
            if (name_offset>=text.size()) throw std::runtime_error("Invalid ELF DVP overlay name");
            const auto end=std::find(text.begin()+name_offset,text.end(),0);
            if (end==text.end()) throw std::runtime_error("Unterminated ELF DVP overlay name");
            const std::string name(text.begin()+name_offset,end);
            unsigned matches=0,extent=0;
            for (unsigned section=0;section<count;++section) {
                const auto header=std::size_t(table)+section*stride;
                if (le32(bytes,header+4)==0x7ffff421U && section_name_at(header)==name) {
                    ++matches; extent=le32(bytes,header+20);
                }
            }
            if (matches!=1 || !extent || extent%8 || source%8 || destination%8 ||
                std::uint64_t(destination)+extent>16384)
                throw std::runtime_error("Invalid ELF DVP overlay extent");
            std::optional<std::uint32_t> file_offset;
            for (const auto& segment:executable.segments)
                if (source>=segment.address && std::uint64_t(source)+extent<=std::uint64_t(segment.address)+segment.file_size) {
                    if (file_offset) throw std::runtime_error("Ambiguous ELF DVP source mapping");
                    file_offset=segment.offset+(source-segment.address);
                }
            if (!file_offset) throw std::runtime_error("ELF DVP overlay source is not file-backed");
            require_range(bytes,*file_offset,extent);
            executable.vector_overlays.push_back({name,source,destination,extent,*file_offset});
        }
    }
    auto& entries=executable.lifecycle_entries;
    std::sort(entries.begin(),entries.end()); entries.erase(std::unique(entries.begin(),entries.end()),entries.end());
    for (const auto entry:entries)
        if (!executable_address(executable,entry)) throw std::runtime_error("ELF lifecycle entry is declared as data");
    for (unsigned index=0;index<count;++index) {
        const auto at=std::size_t(table)+index*stride;
        const auto type=le32(bytes,at+4);
        if (type!=2 && type!=11) continue; // SYMTAB and DYNSYM, not debug labels.
        const auto offset=le32(bytes,at+16),size=le32(bytes,at+20);
        const auto link=le32(bytes,at+24),entry_size=le32(bytes,at+36);
        if (entry_size!=16 || size%16 || size/16>1024*1024 || link>=count ||
            le32(bytes,std::size_t(table)+link*stride+4)!=3)
            throw std::runtime_error("Invalid ELF symbol table layout");
        require_range(bytes,offset,size);
        const auto strings_at=std::size_t(table)+link*stride;
        const auto strings_offset=le32(bytes,strings_at+16),strings_size=le32(bytes,strings_at+20);
        require_range(bytes,strings_offset,strings_size);
        for (std::uint32_t item=0;item<size;item+=16) {
            const auto symbol=std::size_t(offset)+item;
            if ((bytes[symbol+12]&15)!=2) continue; // STT_FUNC only.
            const auto section=le16(bytes,symbol+14);
            if (!section || section>=0xff00) continue; // Undefined and special symbols are not code roots.
            if (section>=count) throw std::runtime_error("Invalid ELF function section index");
            const auto name=le32(bytes,symbol);
            if (name>=strings_size || std::find(bytes.begin()+strings_offset+name,
                    bytes.begin()+strings_offset+strings_size,0)==bytes.begin()+strings_offset+strings_size)
                throw std::runtime_error("Invalid ELF function name");
            const auto declared=std::size_t(table)+section*stride;
            if (le32(bytes,declared+4)!=1 || (le32(bytes,declared+8)&6)!=6) continue;
            const auto address=le32(bytes,symbol+4),extent=std::max(4U,le32(bytes,symbol+8));
            const auto section_address=le32(bytes,declared+12),section_size=le32(bytes,declared+20);
            const auto section_offset=le32(bytes,declared+16);
            if (address%4 || address<section_address ||
                std::uint64_t(address)+extent>std::uint64_t(section_address)+section_size)
                throw std::runtime_error("Invalid ELF function extent");
            // VU functions stay separate even inside an executable LOAD.
            if (!executable_address(executable,address,extent)) continue;
            const auto mapped=std::any_of(executable.segments.begin(),executable.segments.end(),[&](const auto& segment) {
                return address>=segment.address && std::uint64_t(address)+extent<=std::uint64_t(segment.address)+segment.file_size &&
                    std::uint64_t(segment.offset)+address-segment.address==std::uint64_t(section_offset)+address-section_address;
            });
            if (!mapped) throw std::runtime_error("ELF function section is not mapped file data");
            executable.function_entries.push_back(address);
        }
    }
    auto& functions=executable.function_entries;
    std::sort(functions.begin(),functions.end());
    functions.erase(std::unique(functions.begin(),functions.end()),functions.end());
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
        if (le32(b,p)==4) {
            const auto offset=le32(b,p+4),size=le32(b,p+16);
            require_range(b,offset,size);
            import_note(b.subspan(offset,size),result);
        }
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
    read_sections(b,result);
    if (!executable_address(result,result.entry)) throw std::runtime_error("ELF entry is declared as data");
    return result;
}
bool executable_address(const Executable& executable,std::uint32_t address,unsigned bytes,bool allow_inferred) {
    if (!bytes || std::uint64_t(address)+bytes>0x100000000ULL) return false;
    for (const auto& range:executable.non_executable_sections)
        if (address<std::uint64_t(range.address)+range.size && range.address<std::uint64_t(address)+bytes) return false;
    return std::any_of(executable.segments.begin(),executable.segments.end(),[&](const auto& segment) {
        return ((segment.flags&1) || (allow_inferred && executable.inferred_permissions)) && address>=segment.address &&
            std::uint64_t(address)+bytes<=std::uint64_t(segment.address)+segment.file_size;
    });
}
void describe(std::ostream& out, const Executable& executable) {
    out << "MIPS ELF32 entry " << hex32(executable.entry) << " flags " << hex32(executable.flags) << '\n';
    for (const auto& s : executable.segments)
        out << "  LOAD " << hex32(s.address) << " file=" << s.file_size << " memory=" << s.memory_size
            << " flags=" << s.flags << " offset=" << s.offset << '\n';
    out << "Import metadata only; native translation has not been performed.\n";
}
}
