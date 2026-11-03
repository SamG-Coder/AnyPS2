#pragma once
#include <string>
#include <string_view>

namespace nt::inventory::detail {
inline std::string quote(std::string_view text) {
    std::string result="\"";
    constexpr char hex[]="0123456789abcdef";
    for (const unsigned char c:text) {
        if (c=='"' || c=='\\') { result+='\\'; result+=char(c); }
        else if (c<32) { result+="\\u00"; result+=hex[c>>4]; result+=hex[c&15]; }
        else result+=char(c);
    }
    return result+'"';
}
inline std::string table_text(std::string_view text) {
    std::string result;
    for (const char c:text) {
        if (c=='|' || c=='\\' || c=='`' || c=='[' || c==']' || c=='<' || c=='>') result+='\\';
        result+=static_cast<unsigned char>(c)<32 ? ' ' : c;
    }
    return result;
}
}
