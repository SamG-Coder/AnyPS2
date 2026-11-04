#pragma once
#include <array>
#include <cstdint>

namespace nt::media {
// Separable inverse DCT, with Q20 cosine coefficients and one final rounding.
// Products use 64 bits; the MPEG coefficient range [-2048,2047] stays bounded.
// The basis is C(k)/2*cos((2*x+1)*k*pi/16), C(0)=1/sqrt(2).
inline std::array<std::int16_t,64> mpeg_idct(const std::array<std::int16_t,64>& coefficients) {
    static constexpr std::int32_t basis[8][8]{
        {370728,514214,484379,435930,370728,291279,200636,102284},
        {370728,435930,200636,-102284,-370728,-514214,-484379,-291279},
        {370728,291279,-200636,-514214,-370728,102284,484379,435930},
        {370728,102284,-484379,-291279,370728,435930,-200636,-514214},
        {370728,-102284,-484379,291279,370728,-435930,-200636,514214},
        {370728,-291279,-200636,514214,-370728,-102284,484379,-435930},
        {370728,-435930,200636,102284,-370728,514214,-484379,291279},
        {370728,-514214,484379,-435930,370728,-291279,200636,-102284},
    };
    std::array<std::int64_t,64> rows{};
    std::array<std::int16_t,64> result{};
    for (unsigned y=0;y<8;++y) for (unsigned x=0;x<8;++x)
        for (unsigned k=0;k<8;++k) rows[y*8+x]+=std::int64_t(coefficients[y*8+k])*basis[x][k];
    for (unsigned y=0;y<8;++y) for (unsigned x=0;x<8;++x) {
        std::int64_t sum=std::int64_t(1)<<39;
        for (unsigned k=0;k<8;++k) sum+=rows[k*8+x]*basis[y][k];
        result[y*8+x]=static_cast<std::int16_t>(sum>>40);
    }
    return result;
}
}
