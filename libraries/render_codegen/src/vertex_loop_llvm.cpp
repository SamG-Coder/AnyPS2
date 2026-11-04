#include <nt/render_loop.hpp>
#include "float_llvm.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>

namespace nt::render {
namespace {
constexpr std::array<const char*,3> names{"source","output","parameters"};
std::string type(unsigned binding) {
    return "target(\"spirv.VulkanBuffer\", [0 x i32], 12, "+std::to_string(binding==1 ? 1 : 0)+")";
}
std::string suffix(unsigned binding) { return "tspirv.VulkanBuffer_a0i32_12_"+std::to_string(binding==1 ? 1 : 0)+"t"; }
void pointer(std::ostream& out,bool gpu,unsigned binding,const std::string& name,const std::string& index) {
    if (gpu) out<<"  "<<name<<" = call ptr addrspace(11) @llvm.spv.resource.getpointer.p11."
        <<suffix(binding)<<'('<<type(binding)<<" %"<<names[binding]<<", i32 "<<index<<")\n";
    else out<<"  "<<name<<" = getelementptr i32, ptr %"<<names[binding]<<", i32 "<<index<<"\n";
}
void emit(std::ostream& out,const VertexLoop& loop,NumericPolicy policy,bool gpu) {
    if (policy!=NumericPolicy::native_float32) throw std::invalid_argument("Explicit native-float32 render policy required");
    const auto& kernel=loop.kernel();
    validate(kernel);
    out<<"; AnyPS2 bounded vertex loop. Native-float32, not PS2 numerical equivalence.\n"
       <<"; Requires validated bindings and disjoint owned source/parameters/output.\n"
       <<"; No source ISA, register bank, packet stream or helper arithmetic at runtime.\n";
    if (gpu) {
        out<<"target triple = \"spirv-unknown-vulkan1.3\"\n"
           <<"declare i32 @llvm.spv.thread.id.i32(i32)\n";
        for (unsigned b=0;b<3;++b) {
            const std::string name=names[b];
            out<<"@"<<name<<"_name = private constant ["<<name.size()+1<<" x i8] c\""<<name<<"\\00\"\n";
            if (b==2) continue;
            out<<"declare "<<type(b)<<" @llvm.spv.resource.handlefrombinding."<<suffix(b)<<"(i32, i32, i32, i32, ptr)\n"
               <<"declare ptr addrspace(11) @llvm.spv.resource.getpointer.p11."<<suffix(b)<<'('<<type(b)<<", i32)\n";
        }
        out<<"define void @main() #0 {\nentry:\n";
        for (unsigned b=0;b<3;++b)
            out<<"  %"<<names[b]<<" = call "<<type(b)<<" @llvm.spv.resource.handlefrombinding."<<suffix(b)
               <<"(i32 0, i32 "<<b<<", i32 1, i32 0, ptr @"<<names[b]<<"_name)\n";
    } else {
        // Only the binding owner may call this entry. noalias records the
        // disjoint-buffer ABI; it must not be inferred from unrelated guest PCs.
        out<<"define void @nt_render_loop(ptr noalias readonly %source, ptr noalias readonly %parameters, ptr noalias writeonly %output) {\nentry:\n";
    }
    const auto space=gpu ? "addrspace(11) " : "";
    pointer(out,gpu,2,"%count_ptr","0");
    out<<"  %count = load i32, ptr "<<space<<"%count_ptr, align 4\n"
       <<"  %nonzero = icmp ne i32 %count, 0\n"
       <<"  %bounded = icmp ule i32 %count, "<<(loop.unit() ? 1024 : 256)<<"\n"
       <<"  %valid_count = and i1 %nonzero, %bounded\n";
    if (gpu) {
        out<<"  %item = call i32 @llvm.spv.thread.id.i32(i32 0)\n"
           <<"  %in_range = icmp ult i32 %item, %count\n"
           <<"  %active = and i1 %valid_count, %in_range\n";
    }
    out<<"  br i1 "<<(gpu ? "%active" : "%valid_count")<<", label %setup, label %done\nsetup:\n";
    // Hoist invariant uniform values and stream bases before the CPU loop.
    for (unsigned n=0;n<kernel.inputs.size();++n) {
        const auto ptr="%parameter_ptr"+std::to_string(n);
        pointer(out,gpu,2,ptr,std::to_string(n+1));
        out<<"  %parameter"<<n<<" = load i32, ptr "<<space<<ptr<<", align 4\n";
        if (kernel.inputs[n].rate==InputRate::uniform)
            out<<"  "<<detail::value(n)<<" = bitcast i32 %parameter"<<n<<" to float\n";
    }
    out<<"  br label %body\nbody:\n";
    if (!gpu) out<<"  %item = phi i32 [ 0, %setup ], [ %next, %body ]\n";
    for (unsigned n=0;n<kernel.inputs.size();++n) {
        const auto& input=kernel.inputs[n];
        if (input.rate==InputRate::uniform) continue;
        const auto found=std::find_if(loop.reads().begin(),loop.reads().end(),[&](const auto& stream) {
            return stream.vector==input.source.index;
        });
        if (found==loop.reads().end() || !found->stride_rows)
            throw std::invalid_argument("Missing inferred vertex stream");
        const auto index="%source_index"+std::to_string(n),ptr="%source_ptr"+std::to_string(n);
        out<<"  %stride"<<n<<" = mul i32 %item, "<<found->stride_rows*4<<"\n"
           <<"  "<<index<<" = add i32 %parameter"<<n<<", %stride"<<n<<"\n";
        pointer(out,gpu,0,ptr,index);
        out<<"  %bits"<<n<<" = load i32, ptr "<<space<<ptr<<", align 4\n"
           <<"  "<<detail::value(n)<<" = bitcast i32 %bits"<<n<<" to float\n";
    }
    detail::emit_float_arithmetic(out,kernel.arithmetic);
    out<<"  %output_base = mul i32 %item, 4\n";
    for (unsigned n=0;n<4;++n) {
        const auto index="%output_index"+std::to_string(n),ptr="%output_ptr"+std::to_string(n);
        out<<"  "<<index<<" = add i32 %output_base, "<<n<<"\n";
        pointer(out,gpu,1,ptr,index);
        out<<"  %output_bits"<<n<<" = bitcast float "<<detail::value(kernel.arithmetic.outputs[n])<<" to i32\n"
           <<"  store i32 %output_bits"<<n<<", ptr "<<space<<ptr<<", align 4\n";
    }
    if (gpu) out<<"  br label %done\n";
    else out<<"  %next = add i32 %item, 1\n  %more = icmp ult i32 %next, %count\n"
            <<"  br i1 %more, label %body, label %done\n";
    out<<"done:\n  ret void\n}\n";
    if (gpu) out<<"attributes #0 = { \"hlsl.numthreads\"=\"64,1,1\" \"hlsl.shader\"=\"compute\" }\n";
    if (!out) throw std::runtime_error("Cannot emit vertex loop LLVM");
}
}
void emit_cpu_llvm(std::ostream& out,const VertexLoop& loop,NumericPolicy policy) { emit(out,loop,policy,false); }
void emit_spirv_llvm(std::ostream& out,const VertexLoop& loop,NumericPolicy policy) { emit(out,loop,policy,true); }
}
