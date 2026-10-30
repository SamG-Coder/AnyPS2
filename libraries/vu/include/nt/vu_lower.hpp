#pragma once
#include <nt/vu_program.hpp>
#include <string_view>

namespace nt::vu {
enum class LowerOperation { unsupported, literal, branch, integer_add, integer_subtract,
    integer_add_immediate, integer_subtract_immediate, integer_and, integer_or,
    move, rotate, vector_to_integer, integer_to_vector, divide, square_root,
    reciprocal_square_root, wait_quotient, load_quad, store_quad, load_integer, store_integer,
    input_top, input_itop, reciprocal_length, move_p, wait_p, mac_and, mac_or, mac_equal, graphics_kick,
    clip_and, clip_or, clip_equal, clip_set, clip_get };
enum class AddressUpdate { none, increment, decrement };
enum class Bank { none, vector, integer, immediate, quotient };
struct Register {
    Bank bank=Bank::none;
    unsigned index=0,component=4; // 4 denotes the vector rather than one lane.
};
struct Lower {
    LowerOperation operation=LowerOperation::unsupported;
    Register left,right,result;
    unsigned mask=0;
    std::int32_t offset=0;
    std::uint32_t literal=0;
    Control transfer;
    unsigned address_register=0;
    AddressUpdate address_update=AddressUpdate::none;
};
Lower decode_lower(const Pair& pair,unsigned unit);
std::string_view name(LowerOperation operation);
}
