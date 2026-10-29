#pragma once
#include <nt/iop_native_services.hpp>
#include <algorithm>
#include <bit>
#include <charconv>
#include <functional>
#include <string>
#include <string_view>
#include <stdexcept>

namespace nt::iop {
class NativeStdioServices final : public NativeServices {
public:
    using Sink=std::function<bool(std::string_view)>;
    explicit NativeStdioServices(Sink sink) : sink_(std::move(sink)) {
        if (!sink_) throw std::invalid_argument("Native stdio requires an output sink");
    }
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t> ram) override {
        const bool kernel=request.kind==NativeServiceKind::sysmem && request.selector==14;
        if (!kernel && (request.kind!=NativeServiceKind::stdio || request.selector!=4)) return {};
        // The configured host sink replaces diagnostic output. A null Kprintf
        // format is a callback-specific control request, not an empty string.
        if (kernel && !request.arguments[0]) return {};
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
        if (!read_string(request.arguments[0],{},format)) return failure;
        unsigned argument=1;
        const auto next_argument=[&](std::uint32_t& value) {
            if (argument>=128) return false;
            if (argument<4) { value=request.arguments[argument++]; return true; }
            const auto address=std::uint64_t(request.stack_pointer)+argument*4;
            if (address>0xffffffffU) { failure={NativeServiceStatus::address_fault,0,request.stack_pointer}; return false; }
            const auto at=ram_offset(static_cast<std::uint32_t>(address),4,ram.size());
            if (!at) { failure={NativeServiceStatus::address_fault,0,static_cast<std::uint32_t>(address)}; return false; }
            value=load_native_word(ram,*at); ++argument; return true;
        };
        std::string output;
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
                if (!next_argument(value)) return failure;
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
                    if (!next_argument(raw)) return failure;
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
            if (!next_argument(value)) return failure;
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
        // No output is emitted until the entire format and argument set validate.
        if (!sink_(output)) return {NativeServiceStatus::returned,0xffffffff};
        return {NativeServiceStatus::returned,static_cast<std::uint32_t>(output.size())};
    }
private:
    Sink sink_;
};
}
