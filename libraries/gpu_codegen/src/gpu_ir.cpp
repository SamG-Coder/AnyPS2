#include <nt/gpu_ir.hpp>
#include <stdexcept>
#include <string>

namespace nt::gpu {
void validate(const Program& p) {
    if (!p.inputs || p.inputs>4096 || p.expressions.empty() || p.expressions.size()>16384 ||
        p.outputs.empty() || p.outputs.size()>4096)
        throw std::invalid_argument("GPU arithmetic kernel exceeds its bounded interface");
    for (std::size_t n=0;n<p.expressions.size();++n) {
        const auto& e=p.expressions[n];
        switch (e.operation) {
        case Operation::constant: break;
        case Operation::add: case Operation::subtract: case Operation::multiply:
            if (e.left>=p.inputs+n || e.right>=p.inputs+n)
                throw std::invalid_argument("GPU expression contains a forward or invalid value reference");
            break;
        default: throw std::invalid_argument("Unsupported GPU arithmetic operation");
        }
    }
    for (const auto value:p.outputs) if (value>=p.inputs+p.expressions.size())
        throw std::invalid_argument("GPU output references an undefined value");
}
namespace {
constexpr auto input_type="target(\"spirv.VulkanBuffer\", [0 x i32], 12, 0)";
constexpr auto output_type="target(\"spirv.VulkanBuffer\", [0 x i32], 12, 1)";
constexpr auto input_suffix="tspirv.VulkanBuffer_a0i32_12_0t";
constexpr auto output_suffix="tspirv.VulkanBuffer_a0i32_12_1t";
std::string value(std::uint32_t n) { return "%v"+std::to_string(n); }
void emit(std::ostream& out,const Program& p,bool gpu) {
    validate(p);
    const char* convention=gpu ? "spir_func " : "";
    out<<"; AnyPS2 pure arithmetic kernel. Generated ahead of time.\n";
    if (gpu) out<<"target triple = \"spirv-unknown-vulkan1.3\"\n";
    out<<"declare "<<convention<<"i64 @nt_gpu_add(i32, i32, i32)\n"
       <<"declare "<<convention<<"i64 @nt_gpu_multiply(i32, i32)\n";
    if (gpu) {
        out<<"@input_name = private constant [6 x i8] c\"input\\00\"\n"
           <<"@output_name = private constant [7 x i8] c\"output\\00\"\n"
           <<"declare i32 @llvm.spv.thread.id.i32(i32)\n";
        for (const bool write:{false,true}) {
            const auto type=write ? output_type : input_type;
            const auto suffix=write ? output_suffix : input_suffix;
            out<<"declare "<<type<<" @llvm.spv.resource.handlefrombinding."<<suffix<<"(i32, i32, i32, i32, ptr)\n"
               <<"declare ptr addrspace(11) @llvm.spv.resource.getpointer.p11."<<suffix<<"("<<type<<", i32)\n";
        }
        out<<"define void @main() #0 {\nentry:\n"
           <<"  %input = call "<<input_type<<" @llvm.spv.resource.handlefrombinding."<<input_suffix
           <<"(i32 0, i32 0, i32 1, i32 0, ptr @input_name)\n"
           <<"  %output = call "<<output_type<<" @llvm.spv.resource.handlefrombinding."<<output_suffix
           <<"(i32 0, i32 1, i32 1, i32 0, ptr @output_name)\n"
           <<"  %count_ptr = call ptr addrspace(11) @llvm.spv.resource.getpointer.p11."<<input_suffix
           <<"("<<input_type<<" %input, i32 0)\n"
           <<"  %count = load i32, ptr addrspace(11) %count_ptr, align 4\n"
           <<"  %item = call i32 @llvm.spv.thread.id.i32(i32 0)\n";
    } else {
        out<<"define void @nt_gpu_reference(ptr %input, ptr %output, i32 %item) {\nentry:\n"
           <<"  %count = load i32, ptr %input, align 4\n";
    }
    out<<"  %active = icmp ult i32 %item, %count\n"
       <<"  br i1 %active, label %body, label %done\nbody:\n"
       <<"  %input_base0 = mul i32 %item, "<<p.inputs<<"\n"
       <<"  %input_base = add i32 %input_base0, 1\n"
       <<"  %output_base = mul i32 %item, "<<p.outputs.size()+1<<"\n";
    for (std::uint32_t n=0;n<p.inputs;++n) {
        out<<"  %input_index"<<n<<" = add i32 %input_base, "<<n<<"\n";
        if (gpu) out<<"  %input_ptr"<<n<<" = call ptr addrspace(11) @llvm.spv.resource.getpointer.p11."
            <<input_suffix<<"("<<input_type<<" %input, i32 %input_index"<<n<<")\n";
        else out<<"  %input_ptr"<<n<<" = getelementptr i32, ptr %input, i32 %input_index"<<n<<"\n";
        out<<"  "<<value(n)<<" = load i32, ptr "<<(gpu ? "addrspace(11) " : "")<<"%input_ptr"<<n<<", align 4\n";
    }
    std::string status="0";
    for (std::uint32_t n=0;n<p.expressions.size();++n) {
        const auto& e=p.expressions[n]; const auto result=value(p.inputs+n);
        if (e.operation==Operation::constant) { out<<"  "<<result<<" = add i32 0, "<<e.bits<<"\n"; continue; }
        out<<"  %result"<<n<<" = call "<<convention<<"i64 @"
           <<(e.operation==Operation::multiply ? "nt_gpu_multiply" : "nt_gpu_add")
           <<"(i32 "<<value(e.left)<<", i32 "<<value(e.right);
        if (e.operation!=Operation::multiply) out<<", i32 "<<(e.operation==Operation::subtract ? 1 : 0);
        out<<")\n  "<<result<<" = trunc i64 %result"<<n<<" to i32\n"
           <<"  %flags64_"<<n<<" = lshr i64 %result"<<n<<", 32\n"
           <<"  %flags"<<n<<" = trunc i64 %flags64_"<<n<<" to i32\n"
           <<"  %status"<<n<<" = or i32 "<<status<<", %flags"<<n<<"\n";
        status="%status"+std::to_string(n);
    }
    for (std::uint32_t n=0;n<=p.outputs.size();++n) {
        out<<"  %output_index"<<n<<" = add i32 %output_base, "<<n<<"\n";
        if (gpu) out<<"  %output_ptr"<<n<<" = call ptr addrspace(11) @llvm.spv.resource.getpointer.p11."
            <<output_suffix<<"("<<output_type<<" %output, i32 %output_index"<<n<<")\n";
        else out<<"  %output_ptr"<<n<<" = getelementptr i32, ptr %output, i32 %output_index"<<n<<"\n";
        out<<"  store i32 "<<(n ? value(p.outputs[n-1]) : status)<<", ptr "<<(gpu ? "addrspace(11) " : "")
           <<"%output_ptr"<<n<<", align 4\n";
    }
    out<<"  br label %done\ndone:\n  ret void\n}\n";
    if (gpu) out<<"attributes #0 = { \"hlsl.numthreads\"=\"64,1,1\" \"hlsl.shader\"=\"compute\" }\n";
}
}
void emit_spirv_llvm(std::ostream& out,const Program& p) { emit(out,p,true); }
void emit_cpu_llvm(std::ostream& out,const Program& p) { emit(out,p,false); }
}
