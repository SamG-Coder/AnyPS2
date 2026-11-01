#pragma once
#include <nt/iop_codegen.hpp>
#include <iosfwd>
namespace nt::iop_codegen::detail {
void llvm(std::ostream&,const ir::Program&,const CodeStream& code_stream={});
}
