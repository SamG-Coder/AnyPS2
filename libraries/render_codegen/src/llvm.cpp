#include <nt/render_codegen.hpp>
#include <array>
#include <stdexcept>
#include <string>

namespace nt::render {
namespace {
std::string value(unsigned n) { return "%v"+std::to_string(n); }
std::string type(unsigned binding) {
    return "target(\"spirv.VulkanBuffer\", [0 x i32], 12, "+std::to_string(binding)+")";
}
std::string suffix(unsigned binding) { return "tspirv.VulkanBuffer_a0i32_12_"+std::to_string(binding)+"t"; }
constexpr std::array<const char*,3> buffer_names{"input","output","uniforms"};
void pointer(std::ostream& out,bool gpu,unsigned binding,const std::string& name,const std::string& index) {
    if (gpu) out<<"  "<<name<<" = call ptr addrspace(11) @llvm.spv.resource.getpointer.p11."
        <<suffix(binding)<<'('<<type(binding)<<" %"<<buffer_names[binding]<<", i32 "<<index<<")\n";
    else out<<"  "<<name<<" = getelementptr i32, ptr %"<<buffer_names[binding]<<", i32 "<<index<<"\n";
}
void emit(std::ostream& out,const Kernel& kernel,NumericPolicy policy,bool gpu) {
    if (policy!=NumericPolicy::native_float32) throw std::invalid_argument("Explicit native-float32 render policy required");
    const auto sizes=layout(kernel);
    out<<"; AnyPS2 research render kernel; native-float32, not PS2 numeric equivalence.\n"
       <<"; Requires external liveness, independent-item and buffer-extent proofs.\n"
       <<"; Compile-time arithmetic graph only. No runtime source instruction execution.\n";
    if (gpu) {
        out<<"target triple = \"spirv-unknown-vulkan1.3\"\n"
           <<"declare i32 @llvm.spv.thread.id.i32(i32)\n";
        for (unsigned b=0;b<3;++b) {
            if (b==2 && !sizes.uniform_words) continue;
            const std::string name=buffer_names[b];
            out<<"@"<<name<<"_name = private constant ["<<name.size()+1<<" x i8] c\""<<name<<"\\00\"\n"
               <<"declare "<<type(b)<<" @llvm.spv.resource.handlefrombinding."<<suffix(b)<<"(i32, i32, i32, i32, ptr)\n"
               <<"declare ptr addrspace(11) @llvm.spv.resource.getpointer.p11."<<suffix(b)<<'('<<type(b)<<", i32)\n";
        }
        out<<"define void @main() #0 {\nentry:\n";
        for (unsigned b=0;b<3;++b) {
            if (b==2 && !sizes.uniform_words) continue;
            out<<"  %"<<buffer_names[b]<<" = call "<<type(b)<<" @llvm.spv.resource.handlefrombinding."<<suffix(b)
               <<"(i32 0, i32 "<<b<<", i32 1, i32 0, ptr @"<<buffer_names[b]<<"_name)\n";
        }
        pointer(out,true,0,"%count_ptr","0");
        out<<"  %count = load i32, ptr addrspace(11) %count_ptr, align 4\n"
           <<"  %item = call i32 @llvm.spv.thread.id.i32(i32 0)\n";
    } else {
        out<<"define void @nt_render_reference(ptr %input, ptr %uniforms, ptr %output, i32 %item) {\nentry:\n"
           <<"  %count = load i32, ptr %input, align 4\n";
    }
    const auto space=gpu ? "addrspace(11) " : "";
    out<<"  %in_range = icmp ult i32 %item, %count\n"
       <<"  %bounded = icmp ule i32 %count, "<<Layout::maximum_items<<"\n"
       <<"  %active = and i1 %in_range, %bounded\n"
       <<"  br i1 %active, label %body, label %done\nbody:\n"
       <<"  %input_base0 = mul i32 %item, "<<sizes.vertex_words<<"\n"
       <<"  %input_base = add i32 %input_base0, 1\n"
       <<"  %output_base = mul i32 %item, "<<sizes.output_words<<"\n";
    unsigned vertex=0,uniform=0;
    for (unsigned n=0;n<kernel.inputs.size();++n) {
        const auto uniform_input=kernel.inputs[n].rate==InputRate::uniform;
        const auto index="%input_index"+std::to_string(n),ptr="%input_ptr"+std::to_string(n);
        if (uniform_input) {
            pointer(out,gpu,2,ptr,std::to_string(uniform++));
        } else {
            out<<"  "<<index<<" = add i32 %input_base, "<<vertex++<<"\n";
            pointer(out,gpu,0,ptr,index);
        }
        out<<"  %bits"<<n<<" = load i32, ptr "<<space<<ptr<<", align 4\n"
           <<"  "<<value(n)<<" = bitcast i32 %bits"<<n<<" to float\n";
    }
    const auto& program=kernel.arithmetic;
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
    for (unsigned n=0;n<program.outputs.size();++n) {
        const auto index="%output_index"+std::to_string(n),ptr="%output_ptr"+std::to_string(n);
        out<<"  "<<index<<" = add i32 %output_base, "<<n<<"\n";
        pointer(out,gpu,1,ptr,index);
        out<<"  store float "<<value(program.outputs[n])<<", ptr "<<space<<ptr<<", align 4\n";
    }
    out<<"  br label %done\ndone:\n  ret void\n}\n";
    if (gpu) out<<"attributes #0 = { \"hlsl.numthreads\"=\"64,1,1\" \"hlsl.shader\"=\"compute\" }\n";
    if (!out) throw std::runtime_error("Cannot emit render kernel LLVM");
}
std::string lane_name(Lane source) {
    return (source.file==RegisterFile::accumulator ? std::string("acc") : "vf"+std::to_string(source.index))+
        "."+std::string(1,"xyzw"[source.component]);
}
}
void emit_cpu_llvm(std::ostream& out,const Kernel& kernel,NumericPolicy policy) { emit(out,kernel,policy,false); }
void emit_spirv_llvm(std::ostream& out,const Kernel& kernel,NumericPolicy policy) { emit(out,kernel,policy,true); }
void write_manifest(std::ostream& out,const Kernel& kernel) {
    const auto sizes=layout(kernel);
    out<<"{\n  \"schema\": \"anyps2.render-kernel.v1\",\n"
       <<"  \"numeric_policy\": \"native-float32\",\n"
       <<"  \"installable\": false,\n  \"status\": \"research-artifacts-only\",\n"
       <<"  \"source_pairs\": "<<kernel.source_pairs<<",\n"
       <<"  \"live_expressions\": "<<kernel.arithmetic.expressions.size()<<",\n"
       <<"  \"maximum_items\": "<<Layout::maximum_items<<",\n"
       <<"  \"vertex_words\": "<<sizes.vertex_words<<",\n"
       <<"  \"uniform_words\": "<<sizes.uniform_words<<",\n"
       <<"  \"output_words\": "<<sizes.output_words<<",\n  \"inputs\": [\n";
    unsigned vertex=0,uniform=0;
    for (unsigned n=0;n<kernel.inputs.size();++n) {
        const auto& input=kernel.inputs[n]; const bool shared=input.rate==InputRate::uniform;
        out<<"    {\"value\": "<<n<<", \"source\": \""<<lane_name(input.source)
           <<"\", \"rate\": \""<<(shared ? "uniform" : "vertex")<<"\", \"word\": "
           <<(shared ? uniform++ : vertex++)<<"}"<<(n+1==kernel.inputs.size() ? "\n" : ",\n");
    }
    out<<"  ],\n  \"outputs\": [";
    for (unsigned n=0;n<kernel.outputs.size();++n) out<<(n ? ", " : "")<<'"'<<lane_name(kernel.outputs[n])<<'"';
    out<<"],\n  \"unresolved_obligations\": [\n"
       <<"    \"whole-region control and memory dependence proof\",\n"
       <<"    \"all externally observed registers and flags accounted for\",\n"
       <<"    \"independent items and proven uniform provenance\",\n"
       <<"    \"buffer extents, non-aliasing and resource lifetime\",\n"
       <<"    \"native-float32 visual and behavior acceptance\",\n"
       <<"    \"validated SPIR-V and Vulkan execution\",\n"
       <<"    \"GPU-resident draw integration and end-to-end measurement\"\n  ]\n}\n";
    if (!out) throw std::runtime_error("Cannot emit render kernel manifest");
}
}
