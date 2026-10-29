#pragma once
#include <cstdint>
extern "C" std::uint32_t nt_iop_native_code_word(std::uint32_t address);
extern "C" std::uint32_t nt_iop_native_instruction(std::uint32_t address);
