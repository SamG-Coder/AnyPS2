#include <nt/vu_arithmetic.hpp>

namespace nt::vu {
Upper decode_upper(const Operation& fields) {
    Upper result;
    result.left=fields.source; result.right=fields.target;
    result.result=fields.destination; result.mask=fields.mask;
    const auto broadcast=[](unsigned lane) { return static_cast<Operand>(static_cast<unsigned>(Operand::x)+lane); };
    const auto arithmetic=[](unsigned index) {
        switch (index) {
        case 0: return Arithmetic::add;
        case 1: return Arithmetic::subtract;
        case 2: return Arithmetic::multiply_add;
        case 3: return Arithmetic::multiply_subtract;
        case 4: return Arithmetic::maximum;
        case 5: return Arithmetic::minimum;
        case 6: return Arithmetic::multiply;
        default: return Arithmetic::unsupported;
        }
    };
    const auto function=fields.function;
    if (function<28) {
        result.operation=arithmetic(function/4); result.operand=broadcast(function%4);
    } else if (function<32) {
        result.operation=function%2 ? (function==29 ? Arithmetic::maximum : Arithmetic::minimum) : Arithmetic::multiply;
        result.operand=function==28 ? Operand::quotient : Operand::immediate;
    } else if (function<40) {
        result.operation=(function&4) ? ((function&1) ? Arithmetic::multiply_subtract : Arithmetic::subtract) :
            ((function&1) ? Arithmetic::multiply_add : Arithmetic::add);
        result.operand=(function&2) ? Operand::immediate : Operand::quotient;
    } else if (function<48) {
        switch (function) {
        case 40: result.operation=Arithmetic::add; break;
        case 41: result.operation=Arithmetic::multiply_add; break;
        case 42: result.operation=Arithmetic::multiply; break;
        case 43: result.operation=Arithmetic::maximum; break;
        case 44: result.operation=Arithmetic::subtract; break;
        case 45: result.operation=Arithmetic::multiply_subtract; break;
        case 46: result.operation=Arithmetic::outer_subtract; break;
        case 47: result.operation=Arithmetic::minimum; break;
        }
    } else if (function>=60 && function<=63) {
        const auto lane=function-60,sub=fields.subfunction;
        result.destination=Destination::accumulator;
        if (sub<4 || sub==6) {
            result.operation=arithmetic(sub); result.operand=broadcast(lane);
        } else if (sub==4 || sub==5) {
            result.operation=sub==4 ? Arithmetic::integer_to_float : Arithmetic::float_to_integer;
            result.scale=lane==0 ? 0 : lane==1 ? 4 : lane==2 ? 12 : 15;
            result.destination=Destination::vector; result.result=fields.target; result.operand=Operand::none;
        } else if (sub==7) {
            switch (lane) {
            case 0: result.operation=Arithmetic::multiply; result.operand=Operand::quotient; break;
            case 1:
                result.operation=Arithmetic::absolute; result.operand=Operand::none;
                result.destination=Destination::vector; result.result=fields.target; break;
            case 2: result.operation=Arithmetic::multiply; result.operand=Operand::immediate; break;
            case 3: result.operation=Arithmetic::clip; result.operand=Operand::w; result.destination=Destination::none; break;
            }
        } else if (sub==8 || sub==9) {
            result.operation=sub==8 ? ((lane&1) ? Arithmetic::multiply_add : Arithmetic::add) :
                ((lane&1) ? Arithmetic::multiply_subtract : Arithmetic::subtract);
            result.operand=lane<2 ? Operand::quotient : Operand::immediate;
        } else if (sub==10 && lane<3) {
            result.operation=lane==0 ? Arithmetic::add : lane==1 ? Arithmetic::multiply_add : Arithmetic::multiply;
        } else if (sub==11) {
            switch (lane) {
            case 0: result.operation=Arithmetic::subtract; break;
            case 1: result.operation=Arithmetic::multiply_subtract; break;
            case 2: result.operation=Arithmetic::outer_multiply; break;
            case 3: result.operation=Arithmetic::nop; result.destination=Destination::none; result.operand=Operand::none; break;
            }
        }
    }
    return result;
}
std::string_view name(Arithmetic operation) {
    switch (operation) {
    case Arithmetic::nop: return "nop";
    case Arithmetic::add: return "add";
    case Arithmetic::subtract: return "subtract";
    case Arithmetic::multiply: return "multiply";
    case Arithmetic::multiply_add: return "multiply_add";
    case Arithmetic::multiply_subtract: return "multiply_subtract";
    case Arithmetic::maximum: return "maximum";
    case Arithmetic::minimum: return "minimum";
    case Arithmetic::outer_subtract: return "outer_subtract";
    case Arithmetic::outer_multiply: return "outer_multiply";
    case Arithmetic::absolute: return "absolute";
    case Arithmetic::integer_to_float: return "integer_to_float";
    case Arithmetic::float_to_integer: return "float_to_integer";
    case Arithmetic::clip: return "clip";
    default: return "unsupported";
    }
}
}
