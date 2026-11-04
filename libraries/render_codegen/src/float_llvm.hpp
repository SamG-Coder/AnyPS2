#pragma once
#include <nt/gpu_ir.hpp>
#include <ostream>
#include <string>

namespace nt::render::detail {
inline std::string value(unsigned n) { return "%v"+std::to_string(n); }
// Both render ABIs lower the same compact SSA without reassociation or FMA.
inline void emit_float_arithmetic(std::ostream& out,const gpu::Program& program) {
    for (unsigned n=0;n<program.expressions.size();++n) {
        const auto& expression=program.expressions[n];
        out<<"  "<<value(program.inputs+n)<<" = ";
        if (expression.operation==gpu::Operation::constant)
            out<<"bitcast i32 "<<expression.bits<<" to float\n";
        else {
            const auto* operation=expression.operation==gpu::Operation::add ? "fadd" :
                expression.operation==gpu::Operation::subtract ? "fsub" : "fmul";
            out<<operation<<" float "<<value(expression.left)<<", "<<value(expression.right)<<"\n";
        }
    }
}
}
