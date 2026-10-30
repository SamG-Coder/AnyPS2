#pragma once
#include <nt/vu_program.hpp>
#include <string_view>

namespace nt::vu {
enum class Arithmetic { unsupported, nop, add, subtract, multiply, multiply_add,
    multiply_subtract, maximum, minimum, outer_subtract, outer_multiply,
    absolute, integer_to_float, float_to_integer, clip };
enum class Operand { vector, x, y, z, w, immediate, quotient, none };
enum class Destination { vector, accumulator, none };
struct Upper {
    Arithmetic operation=Arithmetic::unsupported;
    Operand operand=Operand::vector;
    Destination destination=Destination::vector;
    unsigned left=0,right=0,result=0,mask=0,scale=0;
};
Upper decode_upper(const Operation& fields);
std::string_view name(Arithmetic operation);
}
