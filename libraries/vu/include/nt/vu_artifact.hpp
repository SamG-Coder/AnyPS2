#pragma once
#include <nt/vu_binding.hpp>

extern "C" const nt::vu::Block* nt_vu_program_blocks();
extern "C" std::uint32_t nt_vu_program_count();
extern "C" const std::uint8_t* nt_vu_program_image();
extern "C" std::uint32_t nt_vu_program_size();
extern "C" std::uint32_t nt_vu_program_destination();
extern "C" std::uint32_t nt_vu_program_unit();
extern "C" const nt::vu::Block* nt_vu_program1_blocks();
extern "C" std::uint32_t nt_vu_program1_count();
extern "C" const std::uint8_t* nt_vu_program1_image();
extern "C" std::uint32_t nt_vu_program1_size();
extern "C" std::uint32_t nt_vu_program1_destination();
extern "C" std::uint32_t nt_vu_program1_unit();

namespace nt::vu {
inline Binding linked_binding() {
    if (nt_vu_program_unit()!=0) throw std::invalid_argument("VU0 artifact reports wrong unit");
    return Binding(Program(nt_vu_program_unit(),{nt_vu_program_blocks(),nt_vu_program_count()}),
        nt_vu_program_destination(),{nt_vu_program_image(),nt_vu_program_size()});
}
inline Binding linked_binding1() {
    if (nt_vu_program1_unit()!=1) throw std::invalid_argument("VU1 artifact reports wrong unit");
    return Binding(Program(1,{nt_vu_program1_blocks(),nt_vu_program1_count()}),
        nt_vu_program1_destination(),{nt_vu_program1_image(),nt_vu_program1_size()});
}
}
