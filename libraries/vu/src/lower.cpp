#include <nt/vu_lower.hpp>

namespace nt::vu {
Lower decode_lower(const Pair& pair,unsigned unit) {
    Lower result;
    result.transfer=control(pair,unit);
    if (pair.immediate) {
        result.operation=LowerOperation::literal; result.literal=*pair.immediate;
        result.result={Bank::immediate}; return result;
    }
    if (!pair.lower) return result;
    const auto& fields=*pair.lower;
    const auto integer=[](unsigned index) { return Register{Bank::integer,index&15U}; };
    const auto vector=[](unsigned index) { return Register{Bank::vector,index}; };
    if (fields.primary==20 || fields.primary==22 || fields.primary==23) {
        result.operation=fields.primary==20 ? LowerOperation::status_equal :
            fields.primary==22 ? LowerOperation::status_and : LowerOperation::status_or;
        result.literal=(fields.encoding&0x7ffU)|((fields.encoding>>10)&0x800U);
        result.result=integer(fields.target);
        return result;
    }
    if ((fields.primary>=16 && fields.primary<=19) || fields.primary==28) {
        result.literal=fields.encoding&0xffffffU;
        switch (fields.primary) {
        case 16: result.operation=LowerOperation::clip_equal; break;
        case 17: result.operation=LowerOperation::clip_set; break;
        case 18: result.operation=LowerOperation::clip_and; break;
        case 19: result.operation=LowerOperation::clip_or; break;
        case 28: result.operation=LowerOperation::clip_get; break;
        }
        if (fields.primary!=17) result.result=integer(fields.primary==28 ? fields.target : 1);
        return result;
    }
    if (fields.primary==24 || fields.primary==26 || fields.primary==27) {
        result.operation=fields.primary==24 ? LowerOperation::mac_equal : fields.primary==26 ? LowerOperation::mac_and : LowerOperation::mac_or;
        result.left=integer(fields.source); result.result=integer(fields.target);
        return result;
    }
    if (fields.primary==4 || fields.primary==5) {
        const bool load=fields.primary==4;
        result.operation=load ? LowerOperation::load_integer : LowerOperation::store_integer;
        result.mask=fields.mask; result.address_register=fields.source&15U;
        if (load) result.result=integer(fields.target); else result.left=integer(fields.target);
        result.offset=static_cast<int>(fields.encoding&0x7ffU)-((fields.encoding&0x400U) ? 2048 : 0);
        return result;
    }
    if (fields.primary==0 || fields.primary==1) {
        const bool load=fields.primary==0;
        result.operation=load ? LowerOperation::load_quad : LowerOperation::store_quad;
        result.mask=fields.mask;
        result.address_register=(load ? fields.source : fields.target)&15U;
        if (load) result.result=vector(fields.target); else result.left=vector(fields.source);
        result.offset=static_cast<int>(fields.encoding&0x7ffU)-((fields.encoding&0x400U) ? 2048 : 0);
        return result;
    }
    if (result.transfer.branch!=Branch::none) {
        result.operation=LowerOperation::branch;
        result.left=integer(result.transfer.source); result.right=integer(result.transfer.target_register);
        if (result.transfer.branch==Branch::call || result.transfer.branch==Branch::register_call)
            result.result=integer(result.transfer.link_register);
        return result;
    }
    if (fields.primary==8 || fields.primary==9) {
        result.operation=fields.primary==8 ? LowerOperation::integer_add_immediate : LowerOperation::integer_subtract_immediate;
        result.left=integer(fields.source); result.result=integer(fields.target);
        result.offset=static_cast<std::int32_t>(((fields.encoding>>10)&0x7800U)|(fields.encoding&0x7ffU));
        return result;
    }
    if (fields.primary!=64) return result;
    if (fields.function<60) {
        result.left=integer(fields.source); result.right=integer(fields.target); result.result=integer(fields.destination);
        switch (fields.function) {
        case 48: result.operation=LowerOperation::integer_add; break;
        case 49: result.operation=LowerOperation::integer_subtract; break;
        case 50:
            result.operation=LowerOperation::integer_add_immediate;
            result.result=integer(fields.target); result.right={};
            result.offset=static_cast<int>(fields.subfunction)-(fields.subfunction&16U ? 32 : 0); break;
        case 52: result.operation=LowerOperation::integer_and; break;
        case 53: result.operation=LowerOperation::integer_or; break;
        }
        return result;
    }
    const auto variant=fields.function-60;
    result.mask=fields.mask;
    if (fields.subfunction==27 && variant==0 && unit==1) {
        result.operation=LowerOperation::graphics_kick; result.left=integer(fields.source); result.mask=0;
    } else if (fields.subfunction==25 && variant==0 && unit==1) {
        result.operation=LowerOperation::move_p; result.result=vector(fields.target);
    } else if (fields.subfunction==28 && variant==3 && unit==1) {
        result.operation=LowerOperation::reciprocal_length; result.left=vector(fields.source); result.mask=0;
    } else if (fields.subfunction==31 && variant==0 && unit==1) {
        result.operation=LowerOperation::sine;
        result.left={Bank::vector,fields.source,(fields.encoding>>21)&3U}; result.mask=0;
    } else if (fields.subfunction==30 && variant==0 && unit==1) {
        result.operation=LowerOperation::efu_square_root;
        result.left={Bank::vector,fields.source,(fields.encoding>>21)&3U}; result.mask=0;
    } else if (fields.subfunction==30 && variant==3 && unit==1) {
        result.operation=LowerOperation::wait_p; result.mask=0;
    } else if (fields.subfunction==26 && variant<2) {
        result.operation=variant ? LowerOperation::input_itop : LowerOperation::input_top;
        result.result=integer(fields.target); result.mask=0;
    } else if (fields.subfunction==12 && variant<2) {
        result.operation=variant ? LowerOperation::rotate : LowerOperation::move;
        result.left=vector(fields.source); result.result=vector(fields.target);
    } else if (fields.subfunction==13) {
        const bool load=(variant&1)==0;
        result.operation=load ? LowerOperation::load_quad : LowerOperation::store_quad;
        result.address_update=variant<2 ? AddressUpdate::increment : AddressUpdate::decrement;
        result.address_register=(load ? fields.source : fields.target)&15U;
        if (load) result.result=vector(fields.target); else result.left=vector(fields.source);
    } else if (fields.subfunction==14) {
        switch (variant) {
        case 0: result.operation=LowerOperation::divide; break;
        case 1: result.operation=LowerOperation::square_root; break;
        case 2: result.operation=LowerOperation::reciprocal_square_root; break;
        case 3: result.operation=LowerOperation::wait_quotient; break;
        }
        result.mask=0;
        if (variant==3) { result.left={Bank::quotient}; return result; }
        if (variant!=1) result.left={Bank::vector,fields.source,(fields.encoding>>21)&3U};
        result.right={Bank::vector,fields.target,(fields.encoding>>23)&3U};
        result.result={Bank::quotient};
    } else if (fields.subfunction==15 && variant>=2) {
        const bool load=variant==2;
        result.operation=load ? LowerOperation::load_integer : LowerOperation::store_integer;
        result.address_register=fields.source&15U;
        if (load) result.result=integer(fields.target); else result.left=integer(fields.target);
    } else if (fields.subfunction==15 && variant<2) {
        if (variant==0) {
            result.operation=LowerOperation::vector_to_integer;
            result.left={Bank::vector,fields.source,(fields.encoding>>21)&3U};
            result.result=integer(fields.target); result.mask=0;
        } else {
            result.operation=LowerOperation::integer_to_vector;
            result.left=integer(fields.source); result.result=vector(fields.target);
        }
    }
    return result;
}
std::string_view name(LowerOperation operation) {
    switch (operation) {
    case LowerOperation::literal: return "literal";
    case LowerOperation::branch: return "branch";
    case LowerOperation::integer_add: return "integer_add";
    case LowerOperation::integer_subtract: return "integer_subtract";
    case LowerOperation::integer_add_immediate: return "integer_add_immediate";
    case LowerOperation::integer_subtract_immediate: return "integer_subtract_immediate";
    case LowerOperation::integer_and: return "integer_and";
    case LowerOperation::integer_or: return "integer_or";
    case LowerOperation::move: return "move";
    case LowerOperation::rotate: return "rotate";
    case LowerOperation::vector_to_integer: return "vector_to_integer";
    case LowerOperation::integer_to_vector: return "integer_to_vector";
    case LowerOperation::divide: return "divide";
    case LowerOperation::square_root: return "square_root";
    case LowerOperation::reciprocal_square_root: return "reciprocal_square_root";
    case LowerOperation::wait_quotient: return "wait_quotient";
    case LowerOperation::load_quad: return "load_quad";
    case LowerOperation::store_quad: return "store_quad";
    case LowerOperation::load_integer: return "load_integer";
    case LowerOperation::store_integer: return "store_integer";
    case LowerOperation::input_top: return "input_top";
    case LowerOperation::input_itop: return "input_itop";
    case LowerOperation::reciprocal_length: return "reciprocal_length";
    case LowerOperation::sine: return "sine";
    case LowerOperation::efu_square_root: return "efu_square_root";
    case LowerOperation::move_p: return "move_p";
    case LowerOperation::wait_p: return "wait_p";
    case LowerOperation::mac_and: return "mac_and";
    case LowerOperation::mac_or: return "mac_or";
    case LowerOperation::mac_equal: return "mac_equal";
    case LowerOperation::graphics_kick: return "graphics_kick";
    case LowerOperation::clip_and: return "clip_and";
    case LowerOperation::clip_or: return "clip_or";
    case LowerOperation::clip_equal: return "clip_equal";
    case LowerOperation::clip_set: return "clip_set";
    case LowerOperation::clip_get: return "clip_get";
    case LowerOperation::status_and: return "status_and";
    case LowerOperation::status_or: return "status_or";
    case LowerOperation::status_equal: return "status_equal";
    default: return "unsupported";
    }
}
}
