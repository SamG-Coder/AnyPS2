#include <nt/loop_pipeline.hpp>
#include <ostream>
#include <stdexcept>
#include <string>

namespace nt::render {
namespace {
using Kind=ElementNodeKind;
using Op=ElementOperation;
constexpr unsigned parameter_first=1,parameter_bases=2,parameter_vectors=21,parameter_accumulator=149,
    parameter_immediate=153,parameter_quotient=154,parameter_p=155;
constexpr const char* bindings[]{"data","output","parameters"};
std::string resource_type(unsigned binding) {
    return "target(\"spirv.VulkanBuffer\", [0 x i32], 12, "+std::to_string(binding==1 ? 1 : 0)+")";
}
std::string resource_suffix(unsigned binding) { return "tspirv.VulkanBuffer_a0i32_12_"+std::to_string(binding==1 ? 1 : 0)+"t"; }
class Emitter {
public:
    Emitter(std::ostream& out,bool vulkan):out_(out),vulkan_(vulkan) {}
    std::string pointer(unsigned binding,const std::string& index) {
        const auto name="%p"+std::to_string(next_++);
        if (vulkan_) out_<<"  "<<name<<" = call ptr addrspace(11) @llvm.spv.resource.getpointer.p11."<<resource_suffix(binding)
                         <<'('<<resource_type(binding)<<" %"<<bindings[binding]<<", i32 "<<index<<")\n";
        else out_<<"  "<<name<<" = getelementptr i32, ptr %"<<bindings[binding]<<", i32 "<<index<<"\n";
        return name;
    }
    std::string load(unsigned binding,const std::string& index) {
        const auto address=pointer(binding,index),name="%l"+std::to_string(next_++);
        out_<<"  "<<name<<" = load i32, ptr "<<(vulkan_ ? "addrspace(11) " : "")<<address<<", align 4\n";
        return name;
    }
    void store(const std::string& index,const std::string& value) {
        const auto address=pointer(1,index);
        out_<<"  store i32 "<<value<<", ptr "<<(vulkan_ ? "addrspace(11) " : "")<<address<<", align 4\n";
    }
    std::string value(const std::string& expression) {
        const auto name="%t"+std::to_string(next_++);
        out_<<"  "<<name<<" = "<<expression<<"\n";
        return name;
    }
private:
    std::ostream& out_;
    bool vulkan_;
    unsigned next_=0;
};
struct Helper { const char* name; unsigned arguments; };
Helper helper(Op op) {
    switch (op) {
    case Op::add: case Op::subtract: return {"nt_gpu_element_add",3};
    case Op::multiply: return {"nt_gpu_element_multiply",2};
    case Op::multiply_add: case Op::multiply_subtract: return {"nt_gpu_element_multiply_accumulate",4};
    case Op::minimum: case Op::maximum: return {"nt_gpu_element_min_max",3};
    case Op::absolute: return {nullptr,1};
    case Op::integer_to_float: return {"nt_gpu_element_integer_to_float",2};
    case Op::float_to_integer: return {"nt_gpu_element_float_to_integer",2};
    case Op::divide: return {"nt_gpu_element_divide",2};
    case Op::square_root: return {"nt_gpu_element_square_root",1};
    case Op::reciprocal_square_root: return {"nt_gpu_element_reciprocal_square_root",2};
    case Op::reciprocal_length: return {"nt_gpu_element_reciprocal_length",3};
    case Op::sine: return {"nt_gpu_element_sine",1};
    case Op::efu_square_root: return {"nt_gpu_element_efu_square_root",1};
    }
    throw std::invalid_argument("Unknown element operation");
}
}
void emit_element_kernel_llvm(std::ostream& out,const PipelinedLoop& loop,unsigned unit,bool vulkan,std::string_view symbol) {
    if (unit>1 || loop.stores.empty()) throw std::invalid_argument("Element kernel needs a recovered loop with stores");
    const auto call_convention=vulkan ? "spir_func " : "";
    out<<"; AnyPS2 recovered VU element kernel. Exact VU arithmetic through nt_gpu_element_* helpers.\n"
       <<"; Requires disjoint owned data/parameters/output buffers and a proven element range.\n";
    if (vulkan) {
        out<<"target triple = \"spirv-unknown-vulkan1.3\"\n"<<"declare i32 @llvm.spv.thread.id.i32(i32)\n";
        for (unsigned b=0;b<3;++b) {
            const std::string name=bindings[b];
            out<<"@"<<name<<"_name = private constant ["<<name.size()+1<<" x i8] c\""<<name<<"\\00\"\n";
            if (b==2) continue; // Shares the read-only resource type of binding 0.
            out<<"declare "<<resource_type(b)<<" @llvm.spv.resource.handlefrombinding."<<resource_suffix(b)<<"(i32, i32, i32, i32, ptr)\n"
               <<"declare ptr addrspace(11) @llvm.spv.resource.getpointer.p11."<<resource_suffix(b)<<'('<<resource_type(b)<<", i32)\n";
        }
    }
    bool declared[16]{};
    for (const auto& node:loop.nodes) if (node.kind==Kind::operation) {
        const auto used=helper(static_cast<Op>(node.a));
        if (!used.name || declared[node.a]) continue;
        // Two operations can share one helper; declare each helper once.
        bool repeated=false;
        for (unsigned other=0;other<16;++other) if (declared[other] && helper(static_cast<Op>(other)).name==std::string(used.name)) repeated=true;
        declared[node.a]=true;
        if (repeated) continue;
        out<<"declare "<<call_convention<<"i64 @"<<used.name<<'(';
        for (unsigned n=0;n<used.arguments;++n) out<<(n ? ", i32" : "i32");
        out<<")\n";
    }
    if (vulkan) {
        out<<"define void @main() #0 {\nentry:\n";
        for (unsigned b=0;b<3;++b)
            out<<"  %"<<bindings[b]<<" = call "<<resource_type(b)<<" @llvm.spv.resource.handlefrombinding."<<resource_suffix(b)
               <<"(i32 0, i32 "<<b<<", i32 1, i32 0, ptr @"<<bindings[b]<<"_name)\n";
    } else out<<"define void @"<<symbol<<"(ptr noalias readonly %data, ptr noalias readonly %parameters, ptr noalias writeonly %output) {\nentry:\n";
    Emitter e(out,vulkan);
    const auto count=e.load(2,"0"),first=e.load(2,std::to_string(parameter_first));
    const auto row_mask=std::to_string(unit ? 1023 : 255);
    if (vulkan) {
        out<<"  %item = call i32 @llvm.spv.thread.id.i32(i32 0)\n"
           <<"  %active = icmp ult i32 %item, "<<count<<"\n  br i1 %active, label %body, label %done\nbody:\n";
    } else {
        out<<"  %any = icmp ne i32 "<<count<<", 0\n  br i1 %any, label %body, label %done\nbody:\n"
           <<"  %item = phi i32 [ 0, %entry ], [ %next, %body ]\n";
    }
    const auto element=e.value("add i32 "+first+", %item");
    std::array<std::string,19> base_rows;
    std::vector<std::string> names(loop.nodes.size());
    std::string failure="0";
    for (std::size_t id=0;id<loop.nodes.size();++id) {
        const auto& node=loop.nodes[id];
        switch (node.kind) {
        case Kind::constant: names[id]=std::to_string(node.value); break;
        case Kind::invariant: {
            const auto index=node.a==vector_file ? parameter_vectors+node.b*4+node.c : node.a==accumulator_file ? parameter_accumulator+node.c :
                node.a==immediate_file ? parameter_immediate : node.a==quotient_file ? parameter_quotient : parameter_p;
            names[id]=e.load(2,std::to_string(index));
            break;
        }
        case Kind::load: {
            if (node.a>18) throw std::invalid_argument("Element load has an invalid base");
            auto& base=base_rows[node.a];
            if (base.empty()) base=node.a==16 ? std::string("0") : e.load(2,std::to_string(parameter_bases+node.a));
            const auto stride=node.a<16 ? loop.strides[node.a] : 0;
            auto row=e.value("add i32 "+base+", "+std::to_string(static_cast<std::int32_t>(node.b)));
            if (stride) row=e.value("add i32 "+row+", "+e.value("mul i32 "+element+", "+std::to_string(stride)));
            const auto wrapped=e.value("and i32 "+row+", "+row_mask);
            names[id]=e.load(0,e.value("add i32 "+e.value("shl i32 "+wrapped+", 2")+", "+std::to_string(node.c)));
            break;
        }
        case Kind::operation: {
            const auto op=static_cast<Op>(node.a);
            const auto operand=[&](std::uint32_t index) { return "i32 "+names[index]; };
            if (op==Op::absolute) { names[id]=e.value("and i32 "+names[node.b]+", 2147483647"); break; }
            std::string arguments;
            switch (op) {
            case Op::add: case Op::subtract: arguments=operand(node.b)+", "+operand(node.c)+", i32 "+(op==Op::subtract ? "1" : "0"); break;
            case Op::multiply_add: case Op::multiply_subtract:
                arguments=operand(node.b)+", "+operand(node.c)+", "+operand(node.d)+", i32 "+(op==Op::multiply_subtract ? "1" : "0"); break;
            case Op::minimum: case Op::maximum: arguments=operand(node.b)+", "+operand(node.c)+", i32 "+(op==Op::maximum ? "1" : "0"); break;
            case Op::integer_to_float: case Op::float_to_integer: arguments=operand(node.b)+", i32 "+std::to_string(node.value); break;
            case Op::square_root: case Op::sine: case Op::efu_square_root: arguments=operand(node.b); break;
            case Op::reciprocal_length: arguments=operand(node.b)+", "+operand(node.c)+", "+operand(node.d); break;
            default: arguments=operand(node.b)+", "+operand(node.c); break;
            }
            const auto packed=e.value("call "+std::string(call_convention)+"i64 @"+helper(op).name+"("+arguments+")");
            names[id]=e.value("trunc i64 "+packed+" to i32");
            failure=e.value("or i32 "+failure+", "+e.value("trunc i64 "+e.value("lshr i64 "+packed+", 63")+" to i32"));
            break;
        }
        case Kind::entry: throw std::invalid_argument("Recovered element function depends on loop entry state");
        }
    }
    const auto words=element_output_words(loop);
    const auto output=e.value("mul i32 %item, "+std::to_string(words));
    e.store(output,failure);
    for (std::size_t n=0;n<loop.stores.size();++n)
        e.store(e.value("add i32 "+output+", "+std::to_string(n+1)),names[loop.stores[n].node]);
    if (vulkan) out<<"  br label %done\n";
    else out<<"  %next = add i32 %item, 1\n  %more = icmp ult i32 %next, "<<count<<"\n  br i1 %more, label %body, label %done\n";
    out<<"done:\n  ret void\n}\n";
    if (vulkan) out<<"attributes #0 = { \"hlsl.numthreads\"=\"64,1,1\" \"hlsl.shader\"=\"compute\" }\n";
    if (!out) throw std::runtime_error("Cannot emit element kernel LLVM");
}
}
