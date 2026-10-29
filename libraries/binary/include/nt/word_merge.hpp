#pragma once
#include <type_traits>

namespace nt {
template<class Word>
inline Word merge_load(Word original, Word memory, unsigned offset, bool left) {
    static_assert(std::is_unsigned_v<Word> && (sizeof(Word) == 4 || sizeof(Word) == 8));
    constexpr auto bits = sizeof(Word) * 8;
    const auto shift = (left ? sizeof(Word) - 1 - offset : offset) * 8;
    if (!shift) return memory;
    if (left) return (original & ((Word(1) << shift) - 1)) | (memory << shift);
    return (original & (Word(~Word(0)) << (bits - shift))) | (memory >> shift);
}
}
