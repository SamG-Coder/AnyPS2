#pragma once
#include <cstdint>
#include <ostream>
#include <span>
#include <vector>

namespace nt::gpu {
// Conversion-time scalar SSA. Values carry encoded 32-bit components, not
// host floats. Runtime code receives compiled shaders, never these operations.
enum class Operation { constant, add, subtract, multiply };
struct Expression {
    Operation operation;
    std::uint32_t left=0,right=0,bits=0;
};
struct Program {
    std::uint32_t inputs=0;
    // Input values precede expressions in the value namespace. References must
    // point backwards; loops must be recovered/outlined before using this IR.
    std::vector<Expression> expressions;
    std::vector<std::uint32_t> outputs;
};
void validate(const Program&);
// Each independent item consumes inputs words and produces outputs words plus
// a status word. Input word zero is the item count; arrays start at word one.
// This is a pure arithmetic kernel, not permission to reorder memory or calls.
void emit_spirv_llvm(std::ostream&,const Program&);
void emit_cpu_llvm(std::ostream&,const Program&);
}
