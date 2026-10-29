#pragma once
#include <nt/iop_native_services.hpp>
#include <bit>
#include <charconv>
#include <string>

namespace nt::iop {
// O32 variadic words follow named parameters, then continue at sp+16.
class NativeFormatArguments {
public:
    NativeFormatArguments(const NativeServiceRequest& request,std::span<const std::uint8_t> ram,unsigned first)
        : request_(request),ram_(ram),argument_(first) {}
    bool operator()(std::uint32_t& value,NativeServiceReply& failure) {
        if (argument_>=128) return false;
        if (argument_<4) { value=request_.arguments[argument_++]; return true; }
        const auto address=std::uint64_t(request_.stack_pointer)+argument_*4;
        const auto at=address<=0xffffffffU ? ram_offset(static_cast<std::uint32_t>(address),4,ram_.size()) : std::nullopt;
        if (!at || ram_.size()-*at<4 || 0x200000-*at<4) {
            failure={NativeServiceStatus::address_fault,0,address<=0xffffffffU ? static_cast<std::uint32_t>(address) : request_.stack_pointer};
            return false;
        }
        value=load_native_word(ram_,*at); ++argument_; return true;
    }
private:
    const NativeServiceRequest& request_;
    std::span<const std::uint8_t> ram_;
    unsigned argument_;
};
// Shared bounded formatting; callers supply their ABI-specific argument reader.
template<class NextArgument>
NativeServiceReply format_native(std::uint32_t format_address,std::span<const std::uint8_t> ram,
                                 NextArgument next_argument,std::string& output) {
    NativeServiceReply failure{};
    const auto read_string=[&](std::uint32_t address,std::optional<unsigned> precision,std::string& out) {
        const auto limit=precision.value_or(4096);
        if (!limit) return true;
        const auto base=ram_offset(address,1,ram.size());
        if (!base) { failure={NativeServiceStatus::address_fault,0,address}; return false; }
        for (unsigned index=0;index<limit;++index) {
            if (index>=ram.size()-*base || index>=0x200000-*base) {
                failure={NativeServiceStatus::address_fault,0,address+index}; return false;
            }
            const auto byte=ram[*base+index];
            if (!byte) return true;
            out.push_back(static_cast<char>(byte));
        }
        return precision.has_value();
    };
    std::string format;
    if (!read_string(format_address,{},format)) return failure;
    for (std::size_t at=0;at<format.size();) {
        if (format[at]!='%') { if (output.size()==16384) return {}; output+=format[at++]; continue; }
        ++at;
        bool left=false,zero=false,plus=false,space=false,alternate=false;
        while (at<format.size()) {
            const auto flag=format[at];
            if (flag=='-') left=true; else if (flag=='0') zero=true; else if (flag=='+') plus=true;
            else if (flag==' ') space=true; else if (flag=='#') alternate=true; else break;
            ++at;
        }
        const auto number=[&](unsigned& result) {
            while (at<format.size() && format[at]>='0' && format[at]<='9') {
                result=result*10+static_cast<unsigned>(format[at++]-'0');
                if (result>4096) return false;
            }
            return true;
        };
        unsigned width=0;
        if (at<format.size() && format[at]=='*') {
            ++at; std::uint32_t value;
            if (!next_argument(value,failure)) return failure;
            const auto signed_width=std::bit_cast<std::int32_t>(value);
            const auto magnitude=signed_width<0 ? -std::int64_t(signed_width) : std::int64_t(signed_width);
            if (magnitude>4096) return {};
            width=static_cast<unsigned>(magnitude); left|=signed_width<0;
        } else if (!number(width)) return {};
        std::optional<unsigned> precision;
        if (at<format.size() && format[at]=='.') {
            ++at; unsigned value=0;
            if (at<format.size() && format[at]=='*') {
                ++at; std::uint32_t raw;
                if (!next_argument(raw,failure)) return failure;
                const auto signed_value=std::bit_cast<std::int32_t>(raw);
                if (signed_value>=0) { if (signed_value>4096) return {}; precision=static_cast<unsigned>(signed_value); }
            } else { if (!number(value)) return {}; precision=value; }
        }
        bool long_word=false;
        if (at<format.size() && format[at]=='l') { ++at; long_word=true; }
        if (at==format.size()) return {};
        const auto spec=format[at++];
        if (spec=='%' && !left && !zero && !plus && !space && !alternate && !width && !precision && !long_word) {
            if (output.size()==16384) return {}; output+='%'; continue;
        }
        const bool numeric=spec=='d' || spec=='i' || spec=='u' || spec=='x' || spec=='X' || spec=='o';
        if (!numeric && (long_word || (spec!='s' && spec!='c') || zero || plus || space || alternate)) return {};
        if (spec=='c' && precision) return {};
        std::uint32_t value;
        if (!next_argument(value,failure)) return failure;
        std::string text,prefix;
        if (spec=='s') { if (!read_string(value,precision,text)) return failure; }
        else if (spec=='c') text+=static_cast<char>(value&255);
        else {
            const auto signed_value=std::bit_cast<std::int32_t>(value);
            const bool sign=spec=='d' || spec=='i';
            const auto magnitude=sign && signed_value<0 ? 0U-value : value;
            char buffer[32];
            const auto converted=std::to_chars(buffer,buffer+sizeof(buffer),magnitude,spec=='o' ? 8 : (spec=='x' || spec=='X') ? 16 : 10);
            text.assign(buffer,converted.ptr);
            if (spec=='X') for (auto& c : text) if (c>='a' && c<='f') c=static_cast<char>(c-'a'+'A');
            if (!magnitude && precision==0) text.clear();
            if (precision && text.size()<*precision) text.insert(0,*precision-text.size(),'0');
            if (sign) prefix=signed_value<0 ? "-" : plus ? "+" : space ? " " : "";
            if (alternate && (spec=='x' || spec=='X') && magnitude) prefix=spec=='x' ? "0x" : "0X";
            if (alternate && spec=='o' && (text.empty() || text.front()!='0')) prefix="0";
        }
        const auto length=prefix.size()+text.size();
        const auto padding=width>length ? width-length : 0;
        if (length+padding>16384-output.size()) return {};
        if (!left && !(numeric && zero && !precision)) output.append(padding,' ');
        output+=prefix;
        if (!left && numeric && zero && !precision) output.append(padding,'0');
        output+=text;
        if (left) output.append(padding,' ');
    }
    return {NativeServiceStatus::returned,static_cast<std::uint32_t>(output.size())};
}
}
