#include <nt/iop_ir.hpp>
#include "constants.hpp"
#include <set>
#include <sstream>
#include <stdexcept>

namespace nt::iop_codegen::ir {
namespace {
std::optional<Operation> native_operation(const elf::IopImport& import) {
    // Minor revisions share an ordinal ABI; only explicitly implemented entries bind.
    if (import.library=="loadcore" && (import.version>>8)==1 && ((import.ordinal>=6 && import.ordinal<=9) || import.ordinal==11 || import.ordinal==12))
        return Operation::native_import;
    if (import.library=="intrman" && (import.version>>8)==1 &&
        ((import.ordinal>=4 && import.ordinal<=9) || import.ordinal==17 || import.ordinal==18 || import.ordinal==23))
        return Operation::native_import;
    if (import.library=="sifcmd" && (import.version>>8)==1 && (import.ordinal==4 || (import.ordinal>=6 && import.ordinal<=14) || import.ordinal==17 || import.ordinal==19 || import.ordinal==20 || import.ordinal==21 || import.ordinal==22 || import.ordinal==23 || import.ordinal==24 || import.ordinal==25 || import.ordinal==28 || import.ordinal==29)) return Operation::native_import;
    if (import.library=="stdio" && (import.version>>8)==1 && import.ordinal==4) return Operation::native_import;
    if (import.library=="sifman" && (import.version>>8)==1 &&
        (import.ordinal==2 || (import.ordinal>=5 && import.ordinal<=8) || (import.ordinal>=21 && import.ordinal<=27) || (import.ordinal>=29 && import.ordinal<=32)))
        return Operation::native_import;
    if (import.library=="thbase" && (import.version>>8)==1 &&
        ((import.ordinal>=4 && import.ordinal<=9) || import.ordinal==14 || import.ordinal==20 || import.ordinal==22 || import.ordinal==41 || (import.ordinal>=33 && import.ordinal<=38) || (import.ordinal>=24 && import.ordinal<=28)))
        return Operation::native_import;
    if ((import.library=="thsemap" || import.library=="thmsgbx") && (import.version>>8)==1 && ((import.ordinal>=4 && import.ordinal<=9) || import.ordinal==11 || import.ordinal==12)) return Operation::native_import;
    if (import.library=="thevent" && (import.version>>8)==1 &&
        ((import.ordinal>=4 && import.ordinal<=11) || import.ordinal==13 || import.ordinal==14))
        return Operation::native_import;
    if (import.library=="ioman" && (import.version>>8)==1 && ((import.ordinal>=4 && import.ordinal<=9) || (import.ordinal>=10 && import.ordinal<=18) || import.ordinal==20 || import.ordinal==21 || import.ordinal==30 || import.ordinal==34))
        return Operation::native_import;
    if (import.library=="sysmem" && (import.version>>8)==1 && ((import.ordinal>=4 && import.ordinal<=10) || import.ordinal==14))
        return Operation::native_import;
    if (import.library!="sysclib" || (import.version>>8)!=1) return {};
    switch (import.ordinal) {
    case 12: return Operation::memory_copy;
    case 13: return Operation::memory_move;
    case 14: return Operation::memory_fill;
    default: return {};
    }
}
Instruction decode(std::uint32_t pc,std::uint32_t word) {
    Instruction i;
    i.address=pc; i.source=(word>>21)&31; i.operand=(word>>16)&31;
    i.destination=(word>>11)&31; i.shift=(word>>6)&31;
    i.immediate=(word&0x8000) ? word|0xffff0000U : word&0xffff;
    const auto op=word>>26;
    if (!op) {
        switch (word&63) {
        case 0: i.operation=Operation::shift_left; break;
        case 2: i.operation=Operation::shift_right; break;
        case 3: i.operation=Operation::shift_arithmetic; break;
        case 4: i.operation=Operation::shift_left_variable; break;
        case 6: i.operation=Operation::shift_right_variable; break;
        case 7: i.operation=Operation::shift_arithmetic_variable; break;
        case 8:
            if (i.operand || i.destination || i.shift) throw std::runtime_error("invalid indirect jump encoding");
            i.operation=i.source==31 ? Operation::return_register : Operation::jump_register;
            i.flow=i.source==31 ? Flow::return_register : Flow::indirect; break;
        case 9:
            if (i.operand || i.shift) throw std::runtime_error("invalid indirect call encoding");
            i.operation=Operation::call_register; i.flow=Flow::indirect_call; break;
        case 12:
            if (word!=12) throw std::runtime_error("unsupported encoded syscall");
            i.operation=Operation::syscall; i.flow=Flow::service; break;
        case 13:
            i.operation=Operation::breakpoint; i.flow=Flow::trap; i.immediate=(word>>6)&0xfffff; break;
        case 16: i.operation=Operation::read_hi; break;
        case 17: i.operation=Operation::write_hi; break;
        case 18: i.operation=Operation::read_lo; break;
        case 19: i.operation=Operation::write_lo; break;
        case 24: i.operation=Operation::multiply_signed; break;
        case 25: i.operation=Operation::multiply_unsigned; break;
        case 26: i.operation=Operation::divide_signed; break;
        case 27: i.operation=Operation::divide_unsigned; break;
        case 32: i.operation=Operation::add_checked; break;
        case 33: i.operation=Operation::add; break;
        case 34: i.operation=Operation::subtract_checked; break;
        case 35: i.operation=Operation::subtract; break;
        case 36: i.operation=Operation::bit_and; break;
        case 37: i.operation=Operation::bit_or; break;
        case 38: i.operation=Operation::bit_xor; break;
        case 39: i.operation=Operation::bit_nor; break;
        case 42: i.operation=Operation::less_signed; break;
        case 43: i.operation=Operation::less_unsigned; break;
        default: throw std::runtime_error("unsupported SPECIAL operation");
        }
        if ((i.operation<=Operation::shift_arithmetic && i.source) ||
            (i.operation>=Operation::shift_left_variable && i.operation<=Operation::shift_arithmetic_variable && i.shift))
            throw std::runtime_error("invalid shift encoding");
    } else if (op==2 || op==3) {
        i.operation=op==2 ? Operation::jump : Operation::call;
        i.flow=op==2 ? Flow::direct : Flow::call;
        i.target=((pc+4)&0xf0000000U)|((word&0x03ffffff)<<2);
    } else if (op==1 || (op>=4 && op<=7)) {
        if (op==1) {
            if (i.operand>1) throw std::runtime_error("unsupported REGIMM branch");
            i.operation=i.operand ? Operation::nonnegative : Operation::negative;
        } else if (op<=5) i.operation=op==4 ? Operation::equal : Operation::not_equal;
        else {
            if (i.operand) throw std::runtime_error("invalid signed branch encoding");
            i.operation=op==6 ? Operation::nonpositive : Operation::positive;
        }
        i.flow=Flow::conditional; i.target=pc+4+(i.immediate<<2);
    } else {
        switch (op) {
        case 8: i.operation=Operation::add_immediate_checked; break;
        case 9: i.operation=Operation::add_immediate; break;
        case 10: i.operation=Operation::less_immediate_signed; break;
        case 11: i.operation=Operation::less_immediate_unsigned; break;
        case 12: i.operation=Operation::and_immediate; break;
        case 13: i.operation=Operation::or_immediate; break;
        case 14: i.operation=Operation::xor_immediate; break;
        case 15: i.operation=Operation::upper_immediate; break;
        case 32: i.operation=Operation::load_byte; i.width=1; break;
        case 33: i.operation=Operation::load_half; i.width=2; break;
        case 35: i.operation=Operation::load_word; i.width=4; break;
        case 36: i.operation=Operation::load_byte_unsigned; i.width=1; break;
        case 37: i.operation=Operation::load_half_unsigned; i.width=2; break;
        case 40: i.operation=Operation::store_byte; i.width=1; break;
        case 41: i.operation=Operation::store_half; i.width=2; break;
        case 43: i.operation=Operation::store_word; i.width=4; break;
        default: throw std::runtime_error("unsupported instruction class");
        }
    }
    return i;
}
}
static Program discover_module(const elf::IopModule& module,std::span<const std::uint32_t> entries,std::span<const elf::IopExports> libraries,
    std::span<const elf::IopModule> data_modules,std::set<std::uint32_t>* external_callbacks=nullptr) {
    const auto extent=module.info ? module.info->text_size : module.initialized_size;
    if (module.base%4 || extent%4 || extent>module.memory.size() || extent>module.initialized_size ||
        module.base>=0x200000 || extent>0x200000-module.base)
        throw std::runtime_error("IOP conversion: invalid executable extent");
    Program program; program.entry=module.entry;
    std::map<std::uint32_t,Instruction> native_imports;
    std::ostringstream unresolved;
    for (const auto& import : elf::iop_imports(module)) {
        auto operation=native_operation(import);
        const bool host=operation.has_value();
        if (!host) for (const auto& library : libraries)
            if (library.name==import.library && (library.version>>8)==(import.version>>8) && import.ordinal<library.functions.size()) {
                operation=Operation::native_import; break;
            }
        if (!operation) {
            unresolved<<"\n  "<<import.library<<" version="<<unsigned(import.version>>8)<<'.'<<unsigned(import.version&255)
                <<" ordinal="<<import.ordinal<<" at "<<hex32(module.base+import.offset);
            continue;
        }
        Instruction call; call.address=module.base+import.offset;
        call.operation=*operation; call.flow=Flow::native_return;
        call.immediate=import.ordinal|(!host ? 0x20000U : import.library=="loadcore" ? 0x10000U : import.library=="ioman" ? 0x30000U : import.library=="intrman" ? 0x40000U : import.library=="thevent" ? 0x50000U : import.library=="thbase" ? 0x60000U : import.library=="sifman" ? 0x70000U : import.library=="stdio" ? 0x80000U : import.library=="sifcmd" ? 0x90000U : import.library=="thsemap" ? 0xa0000U : import.library=="thmsgbx" ? 0xb0000U : 0U);
        native_imports.emplace(call.address,call);
        if (!host) {
            auto bound=import; bound.offset+=module.base; bound.table_offset+=module.base;
            program.library_imports.push_back(std::move(bound));
        }
    }
    if (!unresolved.str().empty())
        throw std::runtime_error("IOP conversion: unsupported native imports:"+unresolved.str());
    std::set<std::uint32_t> pending(entries.begin(),entries.end());
    pending.insert(module.entry);
    for (const auto& table : elf::iop_exports(module))
        pending.insert(table.functions.begin(),table.functions.end());
    std::map<std::uint32_t,Constants> states;
    for (const auto root : pending) states.emplace(root,Constants{});
    const auto enqueue=[&](std::uint32_t target,const Constants& state) {
        const auto [at,inserted]=states.emplace(target,state);
        if (inserted || at->second.merge(state)) pending.insert(target);
    };
    const auto physical_address=[](std::uint32_t address) {
        const auto segment=address&0xe0000000U;
        return (segment==0x80000000U || segment==0xa0000000U) ? address&0x1fffffffU : address;
    };
    const auto discover_callback=[&](std::uint32_t address) {
        const auto target=physical_address(address);
        if (external_callbacks && (target<module.base || target-module.base>=extent)) external_callbacks->insert(target);
        else enqueue(target,Constants{});
    };
    const auto append=[&](std::uint32_t pc) {
        if (pc%4 || pc<module.base || pc-module.base>=extent)
            throw std::runtime_error("IOP conversion: unresolved code target "+hex32(pc));
        if (!program.instructions.contains(pc) && program.instructions.size()>=65536)
            throw std::runtime_error("IOP conversion: 65536-instruction module analysis limit exceeded");
        if (const auto native=native_imports.find(pc);native!=native_imports.end()) {
            program.instructions.emplace(pc,native->second);
            program.protected_words.insert(pc);
            program.protected_words.insert(pc+4);
            return native->second;
        }
        const auto word=le32(module.memory,pc-module.base);
        Instruction instruction;
        try { instruction=decode(pc,word); }
        catch (const std::exception& error) {
            throw std::runtime_error("IOP conversion at "+hex32(pc)+" word="+hex32(word)+": "+error.what());
        }
        program.instructions.emplace(pc,instruction);
        program.protected_words.insert(pc);
        return instruction;
    };
    while (!pending.empty()) {
        const auto pc=*pending.begin(); pending.erase(pending.begin());
        auto instruction=append(pc);
        const auto input=states.at(pc);
        const auto after=transfer(instruction,input);
        if (instruction.flow==Flow::trap) continue;
        if (instruction.flow==Flow::native_return) {
            if (instruction.operation==Operation::native_import && instruction.immediate==0x90011 && input.registers[6])
                for (const auto address : *input.registers[6]) if (address) discover_callback(address);
            if (instruction.operation==Operation::native_import && instruction.immediate==0x9000a && input.registers[5])
                for (const auto address : *input.registers[5]) if (address) discover_callback(address);
            if (instruction.operation==Operation::native_import && instruction.immediate==0x40004 && input.registers[6])
                for (const auto address : *input.registers[6]) discover_callback(address);
            if (instruction.operation==Operation::native_import &&
                (instruction.immediate==0x60023 || instruction.immediate==0x60024) && input.registers[5])
                for (const auto address : *input.registers[5]) discover_callback(address);
            if (instruction.operation==Operation::native_import && instruction.immediate==0x7001e && input.registers[4])
                for (const auto address : *input.registers[4]) if (address) discover_callback(address);
            if (instruction.operation==Operation::native_import && instruction.immediate==0x70020 && input.registers[6])
                for (const auto address : *input.registers[6]) if (address) discover_callback(address);
            if (instruction.operation==Operation::native_import && instruction.immediate==0x30014 && input.registers[4]) {
                const auto initial_word=[&](std::uint32_t address)->std::optional<std::uint32_t> {
                    const auto physical=physical_address(address);
                    if (physical%4) return {};
                    for (const auto& owner : data_modules) {
                        if (physical<owner.base) continue;
                        const auto offset=physical-owner.base;
                        if (offset>owner.initialized_size || owner.initialized_size-offset<4 ||
                            offset>owner.memory.size() || owner.memory.size()-offset<4) continue;
                        return le32(owner.memory,offset);
                    }
                    return {};
                };
                for (const auto descriptor : *input.registers[4]) {
                    if (descriptor>0xffffffffU-16) continue;
                    const auto operations=initial_word(descriptor+16);
                    if (!operations || *operations>0xffffffffU-56) continue;
                    for (const auto offset : {0U,4U,12U,16U,20U,24U,28U,48U,52U,56U}) {
                        const auto function=initial_word(*operations+offset);
                        if (function && *function) discover_callback(*function);
                    }
                    const auto type=initial_word(descriptor+4);
                    if (type && (*type&0xf0000000U)==0x10000000U && *operations<=0xffffffffU-88) {
                        const auto function=initial_word(*operations+88);
                        if (function && *function) discover_callback(*function);
                    }
                }
            }
            if (instruction.operation==Operation::native_import && instruction.immediate==0x60004 && input.registers[4]) {
                for (const auto address : *input.registers[4]) {
                    const auto descriptor=physical_address(address);
                    if (descriptor%4) continue;
                    for (const auto& owner : data_modules) {
                        if (descriptor<owner.base) continue;
                        const auto offset=descriptor-owner.base;
                        if (offset>owner.initialized_size || owner.initialized_size-offset<20 ||
                            offset>owner.memory.size() || owner.memory.size()-offset<20) continue;
                        // Initial data supplies a candidate, never a promise that the
                        // descriptor stays immutable. Runtime creation still checks it.
                        const auto entry=le32(owner.memory,offset+8);
                        if (entry) discover_callback(entry);
                        break;
                    }
                }
            }
            continue;
        }
        if (instruction.flow==Flow::sequential || instruction.flow==Flow::service) { enqueue(pc+4,after); continue; }
        const bool indirect=instruction.flow==Flow::indirect || instruction.flow==Flow::indirect_call;
        if (indirect) {
            const auto& targets=input.registers[instruction.source];
            // An unknown destination must not hide the delay slot or a call's
            // return continuation. Analyse those edges, then reject all remaining
            // unresolved transfers before either backend emits any output.
            std::set<std::uint32_t> physical;
            if (targets) for (const auto target : *targets) physical.insert(physical_address(target));
            instruction.targets.assign(physical.begin(),physical.end());
            program.instructions.at(pc).targets=instruction.targets;
        }
        const auto delay=append(pc+4);
        if (delay.flow==Flow::trap) continue;
        if (delay.flow!=Flow::sequential)
            throw std::runtime_error("IOP conversion: branch in delay slot at "+hex32(pc+4));
        const auto successor=transfer(delay,after);
        if (indirect) {
            for (const auto target : instruction.targets) enqueue(target,successor);
        } else if (instruction.flow!=Flow::return_register) enqueue(instruction.target,successor);
        if (instruction.flow==Flow::conditional) enqueue(pc+8,successor);
        if (instruction.flow==Flow::call || instruction.flow==Flow::indirect_call) enqueue(pc+8,Constants{});
    }
    std::ostringstream indirect_errors;
    for (const auto& [pc,i] : program.instructions)
        if ((i.flow==Flow::indirect || i.flow==Flow::indirect_call) && !states.at(pc).registers[i.source])
            indirect_errors<<"\n  unresolved indirect transfer at "<<hex32(pc)<<" through r"<<i.source;
    if (!indirect_errors.str().empty())
        throw std::runtime_error("IOP conversion: indirect target sets are unknown or exceed the 16-value analysis bound:"+indirect_errors.str());
    return program;
}
Program discover(const elf::IopModule& module,std::span<const std::uint32_t> entries) {
    return discover_module(module,entries,elf::iop_exports(module),std::span(&module,1));
}
Program discover_bundle(std::span<const elf::IopModule> modules,std::span<const std::uint32_t> entries) {
    if (modules.empty()) throw std::runtime_error("IOP conversion: empty module bundle");
    std::vector<elf::IopExports> libraries;
    for (std::size_t index=0;index<modules.size();++index) {
        const auto& module=modules[index];
        const auto end=std::uint64_t(module.base)+module.memory.size();
        if (end>0x200000) throw std::runtime_error("IOP conversion: bundle module exceeds data arena");
        for (std::size_t other=0;other<index;++other)
            if (module.base<std::uint64_t(modules[other].base)+modules[other].memory.size() && modules[other].base<end)
                throw std::runtime_error("IOP conversion: overlapping bundle modules");
        auto exports=elf::iop_exports(module); libraries.insert(libraries.end(),exports.begin(),exports.end());
    }
    std::vector<std::set<std::uint32_t>> roots(modules.size());
    std::set<std::size_t> pending;
    for (std::size_t index=0;index<modules.size();++index) pending.insert(index);
    const auto add_root=[&](std::uint32_t address) {
        for (std::size_t index=0;index<modules.size();++index) {
            const auto& module=modules[index];
            const auto extent=module.info ? module.info->text_size : module.initialized_size;
            if (address%4 || address<module.base || address-module.base>=extent) continue;
            if (roots[index].insert(address).second) pending.insert(index);
            return;
        }
        throw std::runtime_error("IOP conversion: bundle code root has no executable owner at "+hex32(address));
    };
    for (const auto entry : entries) add_root(entry);
    // Callback registration may discover code in an already analyzed module.
    // Revisit only owners with new roots until the bundle reaches a fixed point.
    std::vector<Program> programs(modules.size());
    while (!pending.empty()) {
        const auto index=*pending.begin(); pending.erase(pending.begin());
        const std::vector<std::uint32_t> module_roots(roots[index].begin(),roots[index].end());
        std::set<std::uint32_t> callbacks;
        programs[index]=discover_module(modules[index],module_roots,libraries,modules,&callbacks);
        for (const auto callback : callbacks) add_root(callback);
    }
    Program result; result.entry=modules.front().entry;
    for (const auto& program : programs) {
        result.instructions.insert(program.instructions.begin(),program.instructions.end());
        result.protected_words.insert(program.protected_words.begin(),program.protected_words.end());
        result.library_imports.insert(result.library_imports.end(),program.library_imports.begin(),program.library_imports.end());
    }
    return result;
}
}
