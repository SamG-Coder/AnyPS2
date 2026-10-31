#pragma once
#include <nt/iop_native_services.hpp>
#include <nt/iop_native_code.hpp>
#include <nt/iop_native_format.hpp>
#include <algorithm>
#include <string_view>

namespace nt::iop {
// Bounded native sysclib operations with fixed guest widths and byte semantics.
class NativeSysclibServices final : public NativeServices {
public:
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t> ram) override {
        if (request.kind!=NativeServiceKind::sysclib) return {};
        switch (request.selector) {
        case 24: case 31: case 33: case 34: return string_set(request,ram);
        case 21: case 22: case 25: case 26: case 27: case 29: case 32: return strings(request,ram);
        case 20: case 23: case 30: return copy_string(request,ram);
        case 17: case 41: return fill(request,ram);
        case 19: return format(request,ram);
        case 36: return parse_integer(request,ram);
        default: break;
        }
        if (request.selector!=8) return {};
        const auto byte=request.arguments[0]&255U; // The public argument is char.
        // The public reference defines only ASCII entries. Do not read beyond
        // that domain or silently substitute a locale-dependent classification.
        if (byte>=128) return {NativeServiceStatus::unavailable};
        return {NativeServiceStatus::returned,classify(byte)};
    }
private:
    static NativeServiceReply text(std::uint32_t address,std::span<const std::uint8_t> ram,std::string_view& value) {
        const auto offset=ram_offset(address,1,ram.size());
        if (!offset) return {NativeServiceStatus::address_fault,0,address};
        const auto length=std::min<std::size_t>({0x100000,ram.size()-*offset,0x200000-*offset});
        const auto begin=ram.begin()+*offset, end=begin+length;
        const auto terminator=std::find(begin,end,0);
        if (terminator==end) return {length==0x100000 ? NativeServiceStatus::unavailable : NativeServiceStatus::address_fault,0,address};
        value={reinterpret_cast<const char*>(ram.data()+*offset),static_cast<std::size_t>(terminator-begin)};
        return {NativeServiceStatus::returned};
    }
    static NativeServiceReply string_set(const NativeServiceRequest& request,std::span<const std::uint8_t> ram) {
        std::string_view source,pattern;
        auto checked=text(request.arguments[1],ram,pattern);
        if (checked.status!=NativeServiceStatus::returned) return checked;
        if (request.selector==34 && pattern.empty()) return {NativeServiceStatus::returned,request.arguments[0]};
        checked=text(request.arguments[0],ram,source);
        if (checked.status!=NativeServiceStatus::returned) return checked;
        if (request.selector==34) {
            const auto match=source.find(pattern);
            return {NativeServiceStatus::returned,match==std::string_view::npos ? 0U : request.arguments[0]+static_cast<std::uint32_t>(match)};
        }
        const auto match=request.selector==33 ? source.find_first_not_of(pattern) : source.find_first_of(pattern);
        if (request.selector==31) return {NativeServiceStatus::returned,match==std::string_view::npos ? 0U : request.arguments[0]+static_cast<std::uint32_t>(match)};
        return {NativeServiceStatus::returned,static_cast<std::uint32_t>(match==std::string_view::npos ? source.size() : match)};
    }
    static NativeServiceReply writable(std::uint32_t address,std::uint32_t count,unsigned alignment,
                                       std::span<const std::uint8_t> ram,std::uint32_t& offset) {
        const auto at=ram_offset(address,alignment,ram.size());
        if (!at || count>ram.size()-*at || count>0x200000-*at)
            return {NativeServiceStatus::address_fault,0,address};
        for (auto word=*at&~3U;word<*at+count;word+=4)
            if (nt_iop_native_code_word(word)) return {NativeServiceStatus::code_write,0,address};
        offset=*at; return {NativeServiceStatus::returned,0};
    }
    static NativeServiceReply parse_integer(const NativeServiceRequest& request,std::span<std::uint8_t> ram) {
        const auto source=request.arguments[0],end=request.arguments[1];
        auto base=request.arguments[2];
        std::uint32_t end_offset=0;
        if (end) {
            const auto checked=writable(end,4,4,ram,end_offset);
            if (checked.status!=NativeServiceStatus::returned) return checked;
        }
        const auto finish=[&](std::uint32_t value,std::uint32_t consumed) {
            if (end) store_native_word(ram,end_offset,source+consumed);
            return NativeServiceReply{NativeServiceStatus::returned,value};
        };
        if (base>36) return finish(0,0); // Includes negative signed base values.
        NativeServiceReply failure{NativeServiceStatus::unavailable};
        const auto read=[&](std::uint32_t index,std::uint8_t& byte) {
            if (index>=0x100000) return false;
            const auto address=std::uint64_t(source)+index;
            const auto offset=address<=0xffffffffU ? ram_offset(static_cast<std::uint32_t>(address),1,ram.size()) : std::nullopt;
            if (!offset) {
                failure={NativeServiceStatus::address_fault,0,source}; return false;
            }
            byte=ram[*offset];
            return byte<128; // Public ctype reference defines ASCII only.
        };
        std::uint32_t index=0;
        std::uint8_t byte=0;
        if (!read(index,byte)) return failure;
        while (byte==32 || (byte>=9 && byte<=13)) if (!read(++index,byte)) return failure;
        const bool negative=byte=='-';
        if (negative || byte=='+') if (!read(++index,byte)) return failure;
        if (byte=='0' && (base==0 || base==16 || base==2)) {
            std::uint8_t next=0;
            if (!read(index+1,next)) return failure;
            const bool hex=(base==0 || base==16) && (next=='x' || next=='X');
            const bool binary=(base==0 || base==2) && (next=='b' || next=='B');
            if (hex || binary) {
                base=hex ? 16U : 2U; index+=2;
                if (!read(index,byte)) return failure;
            }
        }
        if (!base) base=byte=='0' ? 8U : 10U;
        const auto limit=negative ? 0x80000000U : 0x7fffffffU;
        std::uint32_t magnitude=0;
        bool digits=false,overflow=false;
        for (;;) {
            const auto digit=byte>='0' && byte<='9' ? unsigned(byte-'0') :
                byte>='a' && byte<='z' ? unsigned(byte-'a'+10) :
                byte>='A' && byte<='Z' ? unsigned(byte-'A'+10) : 36U;
            if (digit>=base) break;
            digits=true;
            const auto next=std::uint64_t(magnitude)*base+digit;
            if (next>limit) overflow=true;
            if (!overflow) magnitude=static_cast<std::uint32_t>(next);
            if (!read(++index,byte)) return failure;
        }
        if (overflow) return finish(limit,digits ? index : 0U);
        return finish(negative ? 0U-magnitude : magnitude,digits ? index : 0U);
    }
    static NativeServiceReply format(const NativeServiceRequest& request,std::span<std::uint8_t> ram) {
        std::string output;
        auto formatted=format_native(request.arguments[1],ram,NativeFormatArguments(request,ram,2),output);
        if (formatted.status!=NativeServiceStatus::returned) {
            if (formatted.status==NativeServiceStatus::unsupported) formatted.status=NativeServiceStatus::unavailable;
            return formatted;
        }
        std::uint32_t destination=0;
        const auto checked=writable(request.arguments[0],static_cast<std::uint32_t>(output.size()+1),1,ram,destination);
        if (checked.status!=NativeServiceStatus::returned) return checked;
        std::copy(output.begin(),output.end(),ram.begin()+destination);
        ram[destination+output.size()]=0;
        return formatted;
    }
    static NativeServiceReply fill(const NativeServiceRequest& request,std::span<std::uint8_t> ram) {
        const bool words=request.selector==41;
        const auto destination=request.arguments[0];
        // _wmemset counts bytes but writes only complete 32-bit words.
        const auto count=words ? request.arguments[2]&~3U : request.arguments[1];
        if (count) {
            std::uint32_t offset=0;
            const auto checked=writable(destination,count,words ? 4 : 1,ram,offset);
            if (checked.status!=NativeServiceStatus::returned) return checked;
            if (words) for (std::uint32_t index=0;index<count;index+=4)
                store_native_word(ram,offset+index,request.arguments[1]);
            else std::fill_n(ram.begin()+offset,count,0);
        }
        return {NativeServiceStatus::returned,words ? destination : 0U};
    }
    static NativeServiceReply copy_string(const NativeServiceRequest& request,std::span<std::uint8_t> ram) {
        const auto original=request.arguments[0],source=request.arguments[1];
        auto destination=original;
        std::uint32_t prefix=0;
        if (request.selector==20) {
            auto query=request; query.selector=27;
            const auto length=strings(query,ram);
            if (length.status!=NativeServiceStatus::returned) return length;
            prefix=length.value;
            if (std::uint64_t(original)+prefix>0xffffffffU) return {NativeServiceStatus::address_fault,0,original};
            destination+=prefix;
        }
        const bool bounded=request.selector==30;
        const auto limit=bounded ? request.arguments[2] : 0x100000U;
        if (limit>0x100000) return {NativeServiceStatus::unavailable};
        if (!limit) return {NativeServiceStatus::returned,destination};
        const auto start=ram_offset(source,1,ram.size());
        if (!start) return {NativeServiceStatus::address_fault,0,source};
        const auto available=std::min<std::size_t>(ram.size()-*start,0x200000-*start);
        std::uint32_t copied=0;
        bool terminated=false;
        while (copied<limit) {
            if (copied>=available) return {NativeServiceStatus::address_fault,0,source};
            if (!ram[*start+copied++]) { terminated=true; break; }
        }
        if (!bounded && !terminated) return {NativeServiceStatus::unavailable};
        const auto count=bounded ? limit : copied;
        const auto output=ram_offset(destination,1,ram.size());
        if (!output || count>ram.size()-*output || count>0x200000-*output)
            return {NativeServiceStatus::address_fault,0,destination};
        // Copy/append overlap is undefined, including overlap with an existing prefix.
        if (*output-prefix<*start+copied && *start<*output+count) return {NativeServiceStatus::unavailable};
        std::uint32_t checked_offset=0;
        const auto checked=writable(destination,count,1,ram,checked_offset);
        if (checked.status!=NativeServiceStatus::returned) return checked;
        std::copy_n(ram.begin()+*start,copied,ram.begin()+*output);
        std::fill_n(ram.begin()+*output+copied,count-copied,0);
        return {NativeServiceStatus::returned,original};
    }
    static NativeServiceReply strings(const NativeServiceRequest& request,std::span<const std::uint8_t> ram) {
        const bool length=request.selector==27,bounded=request.selector==29;
        const bool last=request.selector==26 || request.selector==32;
        const bool search=last || request.selector==21 || request.selector==25;
        std::uint32_t match=0;
        const auto count=bounded ? request.arguments[2] : 0x100000U;
        if (count>0x100000) return {NativeServiceStatus::unavailable};
        NativeServiceReply failure;
        const auto read=[&](std::uint32_t base,std::uint32_t index,std::uint8_t& byte) {
            const auto address=std::uint64_t(base)+index;
            const auto offset=address<=0xffffffffU ? ram_offset(static_cast<std::uint32_t>(address),1,ram.size()) : std::nullopt;
            if (!offset) {
                failure={NativeServiceStatus::address_fault,0,address<=0xffffffffU ? static_cast<std::uint32_t>(address) : base};
                return false;
            }
            byte=ram[*offset]; return true;
        };
        for (std::uint32_t index=0;index<count;++index) {
            std::uint8_t left=0,right=0;
            if (!read(request.arguments[0],index,left)) return failure;
            if (search) {
                if (left==(request.arguments[1]&255U)) {
                    match=request.arguments[0]+index;
                    if (!last) return {NativeServiceStatus::returned,match};
                }
                if (!left) return {NativeServiceStatus::returned,match};
            } else if (length) {
                if (!left) return {NativeServiceStatus::returned,index};
            } else {
                if (!read(request.arguments[1],index,right)) return failure;
                if (left!=right) return {NativeServiceStatus::returned,static_cast<std::uint32_t>(int(left)-int(right))};
                if (!left) return {NativeServiceStatus::returned,0};
            }
        }
        return bounded ? NativeServiceReply{NativeServiceStatus::returned,0} : NativeServiceReply{NativeServiceStatus::unavailable};
    }
    static std::uint32_t classify(std::uint32_t byte) {
        if (byte>=9 && byte<=13) return 8;
        if (byte<32 || byte==127) return 32;
        if (byte==32) return 24;
        if (byte>='0' && byte<='9') return 4;
        if (byte>='A' && byte<='Z') return 1U | (byte<='F' ? 64U : 0U);
        if (byte>='a' && byte<='z') return 2U | (byte<='f' ? 64U : 0U);
        return 16;
    }
};
}
