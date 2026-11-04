#include <nt/element_abi.hpp>

namespace nt::render {
std::vector<std::uint32_t> element_parameters(const ElementEntry& entry,unsigned first_element,unsigned count) {
    std::vector<std::uint32_t> words(element_parameter_words,0);
    words[0]=count; words[1]=first_element;
    for (unsigned n=1;n<16;++n) words[2+n]=entry.integers[n];
    words[2+17]=entry.top; words[2+18]=entry.itop;
    for (unsigned reg=0;reg<32;++reg) for (unsigned lane=0;lane<4;++lane)
        words[21+reg*4+lane]=entry.vectors[reg][lane];
    for (unsigned lane=0;lane<4;++lane) words[149+lane]=entry.accumulator[lane];
    words[153]=entry.immediate; words[154]=entry.quotient; words[155]=entry.p;
    return words;
}
}
