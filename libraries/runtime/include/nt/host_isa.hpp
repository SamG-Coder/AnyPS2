#pragma once
#include <string_view>
#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#include <immintrin.h>
#endif

namespace nt {
// Converted code may be compiled for an x86-64 microarchitecture level
// (clang -march). Programs verify the host before entering that code.
inline bool host_supports_isa(std::string_view level) {
    if (level=="x86-64") return true;
#if defined(_MSC_VER) && defined(_M_X64)
    int base[4]{},extended[4]{},leaf1[4]{},leaf7[4]{},high[4]{};
    __cpuid(base,0); __cpuid(extended,0x80000000);
    if (base[0]>=1) __cpuidex(leaf1,1,0);
    if (base[0]>=7) __cpuidex(leaf7,7,0);
    if (unsigned(extended[0])>=0x80000001U) __cpuid(high,0x80000001);
    const auto bit=[](int word,unsigned index) { return (unsigned(word)>>index)&1U; };
    const bool v2=bit(leaf1[2],0) && bit(leaf1[2],9) && bit(leaf1[2],19) && bit(leaf1[2],20) &&
        bit(leaf1[2],23) && bit(leaf1[2],13) && bit(high[2],0);
    if (level=="x86-64-v2") return v2;
    const bool osxsave=bit(leaf1[2],27);
    const auto state=osxsave ? _xgetbv(0) : 0ULL;
    const bool v3=v2 && osxsave && (state&6)==6 && bit(leaf1[2],28) && bit(leaf1[2],12) && bit(leaf1[2],29) &&
        bit(leaf1[2],22) && bit(leaf7[1],5) && bit(leaf7[1],3) && bit(leaf7[1],8) && bit(high[2],5);
    if (level=="x86-64-v3") return v3;
    const bool v4=v3 && (state&0xe0)==0xe0 && bit(leaf7[1],16) && bit(leaf7[1],17) && bit(leaf7[1],28) &&
        bit(leaf7[1],30) && bit(leaf7[1],31);
    if (level=="x86-64-v4") return v4;
#endif
    return false;
}
}
