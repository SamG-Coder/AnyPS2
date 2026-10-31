#include <nt/iop_codegen.hpp>
#include "program.hpp"
#include <nt/iop_ir.hpp>
#include <sstream>
#include <stdexcept>

namespace nt::iop_codegen {
namespace {
// Textual LLVM IR keeps the converter independent of LLVM's C++ ABI. Clang
// verifies and compiles this module ahead of time; nothing parses it at runtime.
class Writer {
public:
    std::ostringstream text;
    std::string value(const std::string& expression) {
        const auto name="%v"+std::to_string(next_++);
        text<<"  "<<name<<" = "<<expression<<'\n'; return name;
    }
    std::string pointer(const std::string& index) { return value("getelementptr i32, ptr %state, i32 "+index); }
    std::string load(unsigned index) { return value("load i32, ptr "+pointer(std::to_string(index))+", align 4"); }
    std::string reg(unsigned index) { return index ? load(index) : "0"; }
    void store(unsigned index,const std::string& v) {
        const auto ptr=pointer(std::to_string(index));
        text<<"  store i32 "<<v<<", ptr "<<ptr<<", align 4\n";
    }
    std::string binary(const std::string& op,const std::string& a,const std::string& b) {
        return value(op+" i32 "+a+", "+b);
    }
    std::string select(const std::string& condition,const std::string& a,const std::string& b) {
        return value("select i1 "+condition+", i32 "+a+", i32 "+b);
    }
    std::string physical(const std::string& address) {
        const auto segment=binary("and",address,"3758096384");
        const auto cached=binary("icmp eq",segment,"2147483648");
        const auto uncached=binary("icmp eq",segment,"2684354560");
        const auto alias=value("or i1 "+cached+", "+uncached);
        return select(alias,binary("and",address,"536870911"),address);
    }
private:
    unsigned next_=0;
};
void membership(Writer& w,const std::string& name,const std::set<std::uint32_t>& words) {
    std::map<std::uint32_t,std::set<std::uint32_t>> pages;
    for (const auto word : words) pages[word>>10].insert(word);
    for (const auto& [page,instructions] : pages) {
        w.text<<"define internal i1 @"<<name<<"_"<<page<<"(i32 %address) {\nentry:\n  switch i32 %address, label %no [\n";
        for (const auto pc : instructions) w.text<<"    i32 "<<pc<<", label %yes\n";
        w.text<<"  ]\nyes:\n  ret i1 true\nno:\n  ret i1 false\n}\n";
    }
    w.text<<"define internal i1 @"<<name<<"(i32 %address) {\nentry:\n  %page = lshr i32 %address, 10\n"
        "  switch i32 %page, label %no [\n";
    for (const auto& [page,instructions] : pages) w.text<<"    i32 "<<page<<", label %page"<<page<<'\n';
    w.text<<"  ]\nno:\n  ret i1 false\n";
    for (const auto& [page,instructions] : pages)
        w.text<<"page"<<page<<":\n  %member"<<page<<" = call i1 @"<<name<<"_"<<page<<"(i32 %address)\n  ret i1 %member"<<page<<'\n';
    w.text<<"}\n";
}
void memory(Writer& w,const std::string& label,const ir::Instruction& instruction) {
    const auto operation=instruction.operation;
    const bool reverse=instruction.memory_convention==ir::MemoryConvention::source_first;
    const bool words=instruction.memory_convention==ir::MemoryConvention::words;
    const auto delay=w.binary("icmp ne",w.load(34),"0");
    w.text<<"  br i1 "<<delay<<", label %branch_fault, label %fill_start"<<label<<"\nfill_start"<<label<<":\n";
    const auto pending=w.load(35), pending_value=w.load(36);
    const auto reg=[&](unsigned index) { return w.select(w.binary("icmp eq",pending,std::to_string(index)),pending_value,w.load(index)); };
    const auto destination=reg(reverse ? 5 : 4), value=reg(reverse ? 4 : 5), raw_count=reg(6), target=reg(31);
    const auto count=words ? w.binary("and",raw_count,"4294967292") : raw_count;
    const auto empty=w.binary("icmp eq",count,"0");
    w.text<<"  br i1 "<<empty<<", label %fill_commit"<<label<<", label %fill_validate"<<label<<"\nfill_validate"<<label<<":\n";
    const auto offset=w.physical(destination);
    const auto range=w.value("and i1 "+w.binary("icmp ule",offset,"2097152")+", "+w.binary("icmp ule",count,w.binary("sub","2097152",offset)));
    const auto host=w.value("and i1 "+w.binary("icmp ule",offset,"%ram_size")+", "+w.binary("icmp ule",count,w.binary("sub","%ram_size",offset)));
    auto valid=w.value("and i1 "+range+", "+host);
    if (words) valid=w.value("and i1 "+valid+", "+w.binary("icmp eq",w.binary("and",destination,"3"),"0"));
    w.text<<"  br i1 "<<valid<<", label %fill_source"<<label<<", label %fill_fault"<<label<<"\nfill_fault"<<label<<":\n";
    w.store(37,destination); w.text<<"  ret i32 2\nfill_source"<<label<<":\n";
    std::string source;
    if (operation!=ir::Operation::memory_fill) {
        source=w.physical(value);
        const auto source_range=w.value("and i1 "+w.binary("icmp ule",source,"2097152")+", "+w.binary("icmp ule",count,w.binary("sub","2097152",source)));
        const auto source_host=w.value("and i1 "+w.binary("icmp ule",source,"%ram_size")+", "+w.binary("icmp ule",count,w.binary("sub","%ram_size",source)));
        auto source_valid=w.value("and i1 "+source_range+", "+source_host);
        if (words) source_valid=w.value("and i1 "+source_valid+", "+w.binary("icmp eq",w.binary("and",value,"3"),"0"));
        w.text<<"  br i1 "<<source_valid<<", label %copy_overlap"<<label<<", label %source_fault"<<label<<"\nsource_fault"<<label<<":\n";
        w.store(37,value); w.text<<"  ret i32 2\ncopy_overlap"<<label<<":\n";
        if (operation==ir::Operation::memory_copy) {
            const auto separate=w.value("or i1 "+w.binary("icmp uge",source,w.binary("add",offset,count))+", "+w.binary("icmp uge",offset,w.binary("add",source,count)));
            w.text<<"  br i1 "<<separate<<", label %fill_prepare"<<label<<", label %fill_fault"<<label<<'\n';
        } else w.text<<"  br label %fill_prepare"<<label<<'\n';
    } else w.text<<"  br label %fill_prepare"<<label<<'\n';
    w.text<<"fill_prepare"<<label<<":\n";
    const auto first=w.binary("and",offset,"4294967292"), end=w.binary("add",offset,count);
    w.text<<"  br label %fill_check"<<label<<"\nfill_check"<<label<<":\n"
        "  %cursor"<<label<<" = phi i32 [ "<<first<<", %fill_prepare"<<label<<" ], [ %next"<<label<<", %fill_step"<<label<<" ]\n";
    const auto code=w.value("call i1 @is_code(i32 %cursor"+label+")");
    w.text<<"  br i1 "<<code<<", label %fill_code_fault"<<label<<", label %fill_step"<<label<<"\nfill_code_fault"<<label<<":\n";
    w.store(37,destination); w.text<<"  ret i32 4\nfill_step"<<label<<":\n  %next"<<label<<" = add i32 %cursor"<<label<<", 4\n";
    const auto more=w.binary("icmp ult","%next"+label,end);
    w.text<<"  br i1 "<<more<<", label %fill_check"<<label<<", label %fill_write"<<label<<"\nfill_write"<<label<<":\n";
    const auto pointer=w.value("getelementptr i8, ptr %ram, i32 "+offset);
    if (operation==ir::Operation::memory_fill) {
        const auto byte=w.value("trunc i32 "+value+" to i8");
        w.text<<"  call void @llvm.memset.p0.i32(ptr "<<pointer<<", i8 "<<byte<<", i32 "<<count<<", i1 false)\n";
    } else {
        const auto from=w.value("getelementptr i8, ptr %ram, i32 "+source);
        w.text<<"  call void @llvm."<<(operation==ir::Operation::memory_copy ? "memcpy" : "memmove")
            <<".p0.p0.i32(ptr "<<pointer<<", ptr "<<from<<", i32 "<<count<<", i1 false)\n";
    }
    w.text<<"  br label %fill_commit"<<label<<"\nfill_commit"<<label<<":\n";
    const auto retired=w.select(w.binary("icmp eq",pending,"2"),"0",pending);
    const auto load_target=w.pointer(retired);
    w.text<<"  store i32 "<<pending_value<<", ptr "<<load_target<<", align 4\n";
    w.store(0,"0"); w.store(2,destination); w.store(32,target); w.store(35,"0");
    const auto executed=w.value("add i64 "+w.value("load i64, ptr %executed, align 8")+", 1");
    w.text<<"  store i64 "<<executed<<", ptr %executed, align 8\n  br label %loop\n";
}
void instruction(Writer& w,const ir::Instruction& i,bool has_successor,bool open_indirect=false) {
    using ir::Operation;
    const auto label=std::to_string(i.address);
    w.text<<"code"<<label<<":\n";
    const auto left=w.value("load i64, ptr %remaining, align 8");
    const auto empty=w.value("icmp eq i64 "+left+", 0");
    w.text<<"  br i1 "<<empty<<", label %budget_stop, label %execute"<<label<<"\nexecute"<<label<<":\n";
    const auto remaining=w.value("sub i64 "+left+", 1");
    w.text<<"  store i64 "<<remaining<<", ptr %remaining, align 8\n";
    const auto pc=w.load(32);
    if (i.operation==Operation::memory_fill || i.operation==Operation::memory_copy || i.operation==Operation::memory_move) { memory(w,label,i); return; }
    if (i.operation==Operation::native_import) {
        const auto delay=w.binary("icmp ne",w.load(34),"0");
        w.text<<"  br i1 "<<delay<<", label %branch_fault, label %import"<<label<<"\nimport"<<label<<":\n";
        w.store(37,std::to_string(i.immediate)); w.text<<"  ret i32 9\n";
        return;
    }
    if (i.operation==Operation::syscall) {
        w.text<<"  ret i32 6\n";
        return;
    }
    if (i.operation==Operation::breakpoint) {
        w.store(37,std::to_string(i.immediate));
        w.text<<"  ret i32 5\n";
        return;
    }
    const auto a=w.reg(i.source), b=w.reg(i.operand);
    const auto delay=w.load(34), old_target=w.load(33);
    const auto in_delay=w.binary("icmp ne",delay,"0");
    const auto next_pc=w.select(in_delay,old_target,w.binary("add",pc,"4"));
    std::string result, target=old_target, loaded="0";
    unsigned written=0, load_register=0;
    const bool branch=i.flow!=ir::Flow::sequential;
    if (branch) w.text<<"  br i1 "<<in_delay<<", label %branch_fault, label %branch"<<label<<"\nbranch"<<label<<":\n";
    const auto immediate=std::to_string(i.immediate);
    const auto write=[&](unsigned reg,const std::string& v) { written=reg; result=v; };
    const auto retire=[&] {
        const auto pending=w.load(35), pending_value=w.load(36);
        const auto cancel=w.value("or i1 "+w.binary("icmp eq",pending,std::to_string(written))+", "+w.binary("icmp eq",pending,std::to_string(load_register)));
        const auto destination=w.select(cancel,"0",pending);
        const auto pointer=w.pointer(destination);
        w.text<<"  store i32 "<<pending_value<<", ptr "<<pointer<<", align 4\n";
        if (written) w.store(written,result);
        w.store(0,"0"); w.store(32,next_pc); w.store(33,target);
        w.store(34,branch ? "1" : "0"); w.store(35,std::to_string(load_register)); w.store(36,loaded);
        const auto executed=w.value("load i64, ptr %executed, align 8");
        const auto increment=w.value("add i64 "+executed+", 1");
        w.text<<"  store i64 "<<increment<<", ptr %executed, align 8\n";
        if (!has_successor) w.text<<"  br label %loop\n";
        else if (branch) w.text<<"  br label %code"<<i.address+4<<'\n';
        else w.text<<"  br i1 "<<in_delay<<", label %loop, label %code"<<i.address+4<<'\n';
    };
    switch (i.operation) {
    case Operation::add_checked: case Operation::subtract_checked: case Operation::add_immediate_checked: {
        const auto rhs=i.operation==Operation::add_immediate_checked ? immediate : b;
        const std::string intrinsic=i.operation==Operation::subtract_checked ? "llvm.ssub.with.overflow.i32" : "llvm.sadd.with.overflow.i32";
        const auto pair=w.value("call { i32, i1 } @"+intrinsic+"(i32 "+a+", i32 "+rhs+")");
        const auto overflow=w.value("extractvalue { i32, i1 } "+pair+", 1");
        w.text<<"  br i1 "<<overflow<<", label %overflow"<<label<<", label %arithmetic"<<label<<"\noverflow"<<label<<":\n  ret i32 8\narithmetic"<<label<<":\n";
        write(i.operation==Operation::add_immediate_checked ? i.operand : i.destination,w.value("extractvalue { i32, i1 } "+pair+", 0"));
        break;
    }
    case Operation::shift_left: write(i.destination,w.binary("shl",b,std::to_string(i.shift))); break;
    case Operation::shift_right: write(i.destination,w.binary("lshr",b,std::to_string(i.shift))); break;
    case Operation::shift_arithmetic: write(i.destination,w.binary("ashr",b,std::to_string(i.shift))); break;
    case Operation::shift_left_variable: write(i.destination,w.binary("shl",b,w.binary("and",a,"31"))); break;
    case Operation::shift_right_variable: write(i.destination,w.binary("lshr",b,w.binary("and",a,"31"))); break;
    case Operation::shift_arithmetic_variable: write(i.destination,w.binary("ashr",b,w.binary("and",a,"31"))); break;
    case Operation::read_hi: write(i.destination,w.load(38)); break;
    case Operation::read_lo: write(i.destination,w.load(39)); break;
    case Operation::write_hi: w.store(38,a); break;
    case Operation::write_lo: w.store(39,a); break;
    case Operation::multiply_signed: case Operation::multiply_unsigned: {
        const std::string extend=i.operation==Operation::multiply_signed ? "sext" : "zext";
        const auto multiplicand=w.value(extend+" i32 "+a+" to i64"), multiplier=w.value(extend+" i32 "+b+" to i64");
        const auto product=w.value("mul i64 "+multiplicand+", "+multiplier);
        w.store(39,w.value("trunc i64 "+product+" to i32"));
        const auto high=w.value("lshr i64 "+product+", 32");
        w.store(38,w.value("trunc i64 "+high+" to i32"));
        break;
    }
    case Operation::divide_signed: case Operation::divide_unsigned: {
        const bool sign=i.operation==Operation::divide_signed;
        const auto zero=w.binary("icmp eq",b,"0");
        // Mask zero before division, rather than selecting around an invalid
        // division result. Widen signed operands so INT_MIN / -1 is also defined.
        const auto divisor=w.select(zero,"1",b);
        std::string quotient,remainder;
        if (sign) {
            const auto dividend64=w.value("sext i32 "+a+" to i64"), divisor64=w.value("sext i32 "+divisor+" to i64");
            quotient=w.value("trunc i64 "+w.value("sdiv i64 "+dividend64+", "+divisor64)+" to i32");
            remainder=w.value("trunc i64 "+w.value("srem i64 "+dividend64+", "+divisor64)+" to i32");
        } else {
            quotient=w.binary("udiv",a,divisor); remainder=w.binary("urem",a,divisor);
        }
        const auto zero_quotient=sign ? w.select(w.binary("icmp slt",a,"0"),"1","4294967295") : "4294967295";
        w.store(39,w.select(zero,zero_quotient,quotient));
        w.store(38,w.select(zero,a,remainder));
        break;
    }
    case Operation::add: write(i.destination,w.binary("add",a,b)); break;
    case Operation::subtract: write(i.destination,w.binary("sub",a,b)); break;
    case Operation::bit_and: write(i.destination,w.binary("and",a,b)); break;
    case Operation::bit_or: write(i.destination,w.binary("or",a,b)); break;
    case Operation::bit_xor: write(i.destination,w.binary("xor",a,b)); break;
    case Operation::bit_nor: write(i.destination,w.binary("xor",w.binary("or",a,b),"4294967295")); break;
    case Operation::less_unsigned:
        write(i.destination,w.value("zext i1 "+w.binary("icmp ult",a,b)+" to i32")); break;
    case Operation::less_signed:
        write(i.destination,w.value("zext i1 "+w.binary("icmp slt",a,b)+" to i32")); break;
    case Operation::add_immediate: write(i.operand,w.binary("add",a,immediate)); break;
    case Operation::less_immediate_unsigned:
        write(i.operand,w.value("zext i1 "+w.binary("icmp ult",a,immediate)+" to i32")); break;
    case Operation::less_immediate_signed:
        write(i.operand,w.value("zext i1 "+w.binary("icmp slt",a,immediate)+" to i32")); break;
    case Operation::and_immediate: write(i.operand,w.binary("and",a,std::to_string(i.immediate&65535))); break;
    case Operation::or_immediate: write(i.operand,w.binary("or",a,std::to_string(i.immediate&65535))); break;
    case Operation::xor_immediate: write(i.operand,w.binary("xor",a,std::to_string(i.immediate&65535))); break;
    case Operation::upper_immediate: write(i.operand,std::to_string(i.immediate<<16)); break;
    case Operation::return_register: target=a; break;
    case Operation::jump_register: case Operation::call_register: {
        const auto physical=w.physical(a);
        std::string valid="false";
        // An open slot may reach any instruction already compiled into this
        // program. It is not a proof that the stored register is one of them.
        if (open_indirect) valid=w.value("call i1 @is_instruction(i32 "+physical+")");
        else for (const auto destination : i.targets) {
            const auto matches=w.binary("icmp eq",physical,std::to_string(destination));
            valid=valid=="false" ? matches : w.value("or i1 "+valid+", "+matches);
        }
        w.text<<"  br i1 "<<valid<<", label %resolved"<<label<<", label %target_fault"<<label<<"\ntarget_fault"<<label<<":\n";
        w.store(37,a); w.text<<"  ret i32 1\nresolved"<<label<<":\n";
        target=a;
        if (i.operation==Operation::call_register) write(i.destination,w.binary("add",pc,"8"));
        break;
    }
    case Operation::jump: case Operation::call:
        target=w.binary("or",w.binary("and",w.binary("add",pc,"4"),"4026531840"),std::to_string(i.target&0x0fffffff));
        if (i.operation==Operation::call) write(31,w.binary("add",pc,"8"));
        break;
    case Operation::equal: case Operation::not_equal:
        target=w.select(w.binary(i.operation==Operation::equal ? "icmp eq" : "icmp ne",a,b),
            w.binary("add",pc,std::to_string(4+(i.immediate<<2))),w.binary("add",pc,"8"));
        break;
    case Operation::negative: case Operation::nonnegative: case Operation::nonpositive: case Operation::positive: {
        const auto predicate=i.operation==Operation::negative ? "icmp slt" : i.operation==Operation::nonnegative ? "icmp sge" :
            i.operation==Operation::nonpositive ? "icmp sle" : "icmp sgt";
        target=w.select(w.binary(predicate,a,"0"),w.binary("add",pc,std::to_string(4+(i.immediate<<2))),w.binary("add",pc,"8"));
        break;
    }
    default: {
        if (!i.width) throw std::runtime_error("LLVM backend: unsupported operation");
        const bool merge_load=i.operation==Operation::load_word_left || i.operation==Operation::load_word_right;
        const bool merge_store=i.operation==Operation::store_word_left || i.operation==Operation::store_word_right;
        const bool merge=merge_load || merge_store;
        const auto address=w.binary("add",a,immediate), physical_address=w.physical(address);
        const auto offset=merge ? w.binary("and",physical_address,"4294967292") : physical_address;
        const auto aligned=merge ? "true" : w.binary("icmp eq",w.binary("and",address,std::to_string(i.width-1)),"0");
        const auto end=w.binary("add",offset,std::to_string(i.width));
        const auto physical=w.binary("icmp ule",offset,std::to_string(0x200000-i.width));
        const auto within=w.binary("icmp ule",end,"%ram_size");
        w.text<<"  br i1 "<<aligned<<", label %aligned"<<label<<", label %fault"<<label<<"\naligned"<<label<<":\n";
        w.text<<"  br i1 "<<physical<<", label %ram"<<label<<", label %mmio"<<label<<"\nram"<<label<<":\n";
        w.text<<"  br i1 "<<within<<", label %memory"<<label<<", label %fault"<<label<<"\nmmio"<<label<<":\n";
        // Return typed access metadata to the session-owned native service.
        // No guest instruction is fetched or decoded by the service dispatcher.
        const bool sio2_word=!merge && (i.operation==Operation::load_word || i.operation==Operation::store_word);
        const bool sio2_byte=!merge && (i.operation==Operation::load_byte_unsigned || i.operation==Operation::store_byte);
        if (!sio2_word && !sio2_byte) w.text<<"  br label %fault"<<label<<"\n";
        else {
            constexpr std::uint32_t sio2_base=0x1f808200;
            const auto above=w.binary("icmp uge",offset,std::to_string(sio2_base));
            const auto below=w.binary("icmp ule",offset,std::to_string(sio2_base+32*4));
            const auto inside=sio2_byte ? w.binary("icmp eq",offset,std::to_string(sio2_base+
                (i.operation==Operation::store_byte ? 0x60 : 0x64))) : w.value("and i1 "+above+", "+below);
            w.text<<"  br i1 "<<inside<<", label %mmio_hit"<<label<<", label %fault"<<label<<"\nmmio_hit"<<label<<":\n";
            const auto relative=w.binary("sub",offset,std::to_string(sio2_base));
            const auto access=w.binary("or",w.binary("shl",relative,"16"),
                std::to_string(i.operand|((i.operation==Operation::store_word || i.operation==Operation::store_byte) ? 256U : 0U)|
                    (sio2_byte ? 512U : 0U)));
            w.store(37,access);
            w.text<<"  ret i32 12\n";
        }
        w.text<<"fault"<<label<<":\n";
        w.store(37,address); w.text<<"  ret i32 2\nmemory"<<label<<":\n";
        const auto ptr=w.value("getelementptr i8, ptr %ram, i32 "+offset);
        const auto type="i"+std::to_string(i.width*8);
        const auto merge_word=[&](const std::string& memory,const std::string& original) {
            const bool left=i.operation==Operation::load_word_left || i.operation==Operation::store_word_left;
            const auto lane=w.binary("and",address,"3");
            const auto shift=w.binary("shl",left ? w.binary("sub","3",lane) : lane,"3");
            const std::string direction=(left==merge_load) ? "shl" : "lshr";
            const auto mask=w.binary(direction,"4294967295",shift);
            const auto retained=w.binary("and",merge_load ? original : memory,w.binary("xor",mask,"4294967295"));
            return w.binary("or",retained,w.binary(direction,merge_load ? memory : original,shift));
        };
        if (i.operation<=Operation::load_half_unsigned || merge_load) {
            loaded=w.value("load "+type+", ptr "+ptr+", align 1");
            if (i.width<4) {
                const bool sign=i.operation==Operation::load_byte || i.operation==Operation::load_half;
                loaded=w.value(std::string(sign ? "sext " : "zext ")+type+" "+loaded+" to i32");
            }
            if (merge_load) {
                // Adjacent LWL/LWR forward the pending value for the same
                // destination; ordinary instructions still see the old register.
                const auto original=i.operand ? w.select(w.binary("icmp eq",w.load(35),std::to_string(i.operand)),w.load(36),b) : "0";
                loaded=merge_word(loaded,original);
            }
            load_register=i.operand;
        } else {
            const auto code=w.value("call i1 @is_code(i32 "+w.binary("and",offset,"4294967292")+")");
            w.text<<"  br i1 "<<code<<", label %write_fault"<<label<<", label %store"<<label<<"\nwrite_fault"<<label<<":\n";
            w.store(37,address); w.text<<"  ret i32 4\nstore"<<label<<":\n";
            const auto narrowed=merge_store ? merge_word(w.value("load i32, ptr "+ptr+", align 1"),b) :
                i.width==4 ? b : w.value("trunc i32 "+b+" to "+type);
            w.text<<"  store "<<type<<' '<<narrowed<<", ptr "<<ptr<<", align 1\n";
        }
        break;
    }
    }
    // Retire only after every possible fault. Old operands were read before a
    // pending load is committed, and a newer write cancels that pending load.
    // Serial requests return before this point; the native service retires them.
    retire();
}
}
void emit_llvm(std::ostream& output,const elf::IopModule& module,std::span<const std::uint32_t> entries) {
    detail::llvm(output,ir::discover(module,entries));
}
void emit_program(std::ostream& output,const ir::Program& program) {
    if (program.instructions.empty()) throw std::runtime_error("IOP conversion: empty program");
    detail::llvm(output,program);
}
void detail::llvm(std::ostream& output,const ir::Program& program) {
    // Fixed address pages bound execution functions to at most 256 instructions.
    std::map<std::uint32_t,std::map<std::uint32_t,ir::Instruction>> pages;
    for (const auto& [pc,i] : program.instructions) pages[pc>>10].emplace(pc,i);
    Writer w;
    w.text<<"declare { i32, i1 } @llvm.sadd.with.overflow.i32(i32, i32)\n"
            "declare { i32, i1 } @llvm.ssub.with.overflow.i32(i32, i32)\n"
            "declare void @llvm.memset.p0.i32(ptr, i8, i32, i1)\n"
            "declare void @llvm.memcpy.p0.p0.i32(ptr, ptr, i32, i1)\n"
            "declare void @llvm.memmove.p0.p0.i32(ptr, ptr, i32, i1)\n";
    w.text<<"; AnyPS2 ahead-of-time IOP output. Keep commercial conversions out of source control.\n";
    std::set<std::uint32_t> entries_set;
    for (const auto& [pc,i] : program.instructions) entries_set.insert(pc);
    membership(w,"is_instruction",entries_set);
    membership(w,"is_code",program.protected_words);
    w.text<<"define i32 @nt_iop_native_instruction(i32 %address) {\nentry:\n"
        "  %member = call i1 @is_instruction(i32 %address)\n  %result = zext i1 %member to i32\n  ret i32 %result\n}\n";
    w.text<<"define i32 @nt_iop_native_code_word(i32 %address) {\nentry:\n"
        "  %member = call i1 @is_code(i32 %address)\n  %result = zext i1 %member to i32\n  ret i32 %result\n}\n";
    for (const auto& [page,instructions] : pages) {
        w.text<<"define internal i32 @run_page_"<<page<<"(ptr %state, ptr %ram, i32 %ram_size, ptr %executed, ptr %remaining) noinline {\n"
            "entry:\n  br label %loop\n"
            "loop:\n  %left = load i64, ptr %remaining, align 8\n  %done = icmp eq i64 %left, 0\n"
            "  br i1 %done, label %budget_stop, label %dispatch\ndispatch:\n"
            "  %pc_ptr = getelementptr i32, ptr %state, i32 32\n  %pc = load i32, ptr %pc_ptr, align 4\n";
        const auto pc=w.physical("%pc");
        w.text<<"  switch i32 "<<pc<<", label %redispatch [\n";
        for (const auto& [address,i] : instructions) w.text<<"    i32 "<<address<<", label %code"<<address<<'\n';
        w.text<<"  ]\nbudget_stop:\n  ret i32 0\nredispatch:\n  ret i32 7\nbranch_fault:\n  ret i32 3\n";
        for (const auto& [address,i] : instructions)
            instruction(w,i,instructions.contains(address+4),program.open_indirect_sites.contains(address));
        w.text<<"}\n";
    }
    w.text<<"define i32 @nt_iop_llvm_run_v2(ptr %state, ptr %ram, i32 %ram_size, ptr %executed, i64 %budget) {\n"
        "entry:\n  %remaining = alloca i64, align 8\n  store i64 %budget, ptr %remaining, align 8\n  br label %loop\n"
        "loop:\n  %left = load i64, ptr %remaining, align 8\n  %done = icmp eq i64 %left, 0\n"
        "  br i1 %done, label %budget_stop, label %dispatch\ndispatch:\n"
        "  %pc_ptr = getelementptr i32, ptr %state, i32 32\n  %pc = load i32, ptr %pc_ptr, align 4\n";
    const auto pc=w.physical("%pc");
    const auto member=w.value("call i1 @is_instruction(i32 "+pc+")");
    w.text<<"  br i1 "<<member<<", label %route, label %unmapped\nroute:\n";
    const auto page=w.binary("lshr",pc,"10");
    w.text<<"  switch i32 "<<page<<", label %unmapped [\n";
    for (const auto& [number,instructions] : pages) w.text<<"    i32 "<<number<<", label %page"<<number<<'\n';
    w.text<<"  ]\nbudget_stop:\n  ret i32 0\nunmapped:\n  ret i32 1\n";
    for (const auto& [number,instructions] : pages) {
        w.text<<"page"<<number<<":\n";
        const auto result=w.value("call i32 @run_page_"+std::to_string(number)+"(ptr %state, ptr %ram, i32 %ram_size, ptr %executed, ptr %remaining)");
        const auto again=w.binary("icmp eq",result,"7");
        w.text<<"  br i1 "<<again<<", label %loop, label %stop"<<number<<"\nstop"<<number<<":\n  ret i32 "<<result<<'\n';
    }
    w.text<<"}\n";
    output<<w.text.str();
}
}
