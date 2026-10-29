#pragma once
#include <nt/iop_ir.hpp>
#include <iosfwd>
namespace nt::iop_codegen::detail {
void llvm(std::ostream&,const ir::Program&);
}
