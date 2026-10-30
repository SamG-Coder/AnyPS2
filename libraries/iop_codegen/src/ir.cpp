#include <nt/iop_ir.hpp>
#include "constants.hpp"
#include "jump_tables.hpp"
#include "loaded_pointers.hpp"
#include "returned_addresses.hpp"
#include "stack_callbacks.hpp"
#include <set>
#include <sstream>
#include <stdexcept>

namespace nt::iop_codegen::ir {
namespace {
std::optional<Operation> native_operation(const elf::IopImport& import) {
    if (import.library=="romdrv" && (import.version>>8)==2 && (import.ordinal==4 || import.ordinal==5))
        return Operation::native_import;
    // Binding identifies the host ABI, not successful implementation. Module
    // lifecycle operations remain explicit runtime stops without a provider.
    if (import.library=="modload" && (import.version>>8)==1 && import.ordinal>=6 && import.ordinal<=23)
        return Operation::native_import;
    // Minor revisions share an ordinal ABI; only explicitly implemented entries bind.
    if (import.library=="loadcore" && (import.version>>8)==1 && ((import.ordinal>=4 && import.ordinal<=10) || import.ordinal==11 || import.ordinal==12 || import.ordinal==20 || import.ordinal==27))
        return Operation::native_import;
    if (import.library=="intrman" && (import.version>>8)==1 &&
        ((import.ordinal>=4 && import.ordinal<=9) || import.ordinal==14 || import.ordinal==17 || import.ordinal==18 || import.ordinal==23))
        return Operation::native_import;
    if (import.library=="sifcmd" && (import.version>>8)==1 && (import.ordinal==4 || (import.ordinal>=6 && import.ordinal<=14) || import.ordinal==17 || import.ordinal==19 || import.ordinal==20 || import.ordinal==21 || import.ordinal==22 || import.ordinal==23 || import.ordinal==24 || import.ordinal==25 || import.ordinal==28 || import.ordinal==29)) return Operation::native_import;
    if (import.library=="stdio" && (import.version>>8)==1 && import.ordinal==4) return Operation::native_import;
    if (import.library=="sifman" && (import.version>>8)==1 &&
        (import.ordinal==2 || (import.ordinal>=5 && import.ordinal<=8) || (import.ordinal>=21 && import.ordinal<=27) || (import.ordinal>=29 && import.ordinal<=32)))
        return Operation::native_import;
    if (import.library=="thbase" && (import.version>>8)==1 &&
        ((import.ordinal>=4 && import.ordinal<=10) || import.ordinal==14 || import.ordinal==20 || import.ordinal==21 || import.ordinal==22 || import.ordinal==41 || (import.ordinal>=33 && import.ordinal<=38) || (import.ordinal>=24 && import.ordinal<=28)))
        return Operation::native_import;
    if ((import.library=="thsemap" || import.library=="thmsgbx") && (import.version>>8)==1 && ((import.ordinal>=4 && import.ordinal<=9) || import.ordinal==11 || import.ordinal==12)) return Operation::native_import;
    if (import.library=="thevent" && (import.version>>8)==1 &&
        ((import.ordinal>=4 && import.ordinal<=11) || import.ordinal==13 || import.ordinal==14))
        return Operation::native_import;
    if (import.library=="ioman" && (import.version>>8)==1 && ((import.ordinal>=4 && import.ordinal<=9) || (import.ordinal>=10 && import.ordinal<=18) || import.ordinal==20 || import.ordinal==21 || import.ordinal==22 || import.ordinal==25 || import.ordinal==26 || import.ordinal==27 || import.ordinal==28 || import.ordinal==29 || import.ordinal==30 || import.ordinal==31 || import.ordinal==32 || import.ordinal==33 || import.ordinal==34))
        return Operation::native_import;
    if (import.library=="sysmem" && (import.version>>8)==1 && ((import.ordinal>=4 && import.ordinal<=10) || import.ordinal==14))
        return Operation::native_import;
    if (import.library=="heaplib" && (import.version>>8)==1 && import.ordinal>=4 && import.ordinal<=8) return Operation::native_import;
    // Binding identifies the host ABI. Slice setup and transfer starts remain
    // explicit runtime stops and do not report a completed DMA operation.
    if (import.library=="dmacman" && (import.version>>8)==1 &&
        ((import.ordinal>=4 && import.ordinal<=11) || (import.ordinal>=14 && import.ordinal<=17) ||
         import.ordinal==26 || import.ordinal==27 || import.ordinal==28 || import.ordinal==32 ||
         (import.ordinal>=33 && import.ordinal<=35)))
        return Operation::native_import;
    if (import.library!="sysclib" || (import.version>>8)!=1) return {};
    switch (import.ordinal) {
    case 8: case 17: case 18: case 19: case 20: case 21: case 22: case 23: case 25: case 26: case 27: case 29: case 30: case 32: case 36: case 41: return Operation::native_import;
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
        case 34: i.operation=Operation::load_word_left; i.width=4; break;
        case 35: i.operation=Operation::load_word; i.width=4; break;
        case 36: i.operation=Operation::load_byte_unsigned; i.width=1; break;
        case 37: i.operation=Operation::load_half_unsigned; i.width=2; break;
        case 38: i.operation=Operation::load_word_right; i.width=4; break;
        case 40: i.operation=Operation::store_byte; i.width=1; break;
        case 41: i.operation=Operation::store_half; i.width=2; break;
        case 42: i.operation=Operation::store_word_left; i.width=4; break;
        case 43: i.operation=Operation::store_word; i.width=4; break;
        case 46: i.operation=Operation::store_word_right; i.width=4; break;
        default: throw std::runtime_error("unsupported instruction class");
        }
    }
    return i;
}
}
static bool note_gap(std::vector<Gap>* gaps,Gap gap) {
    if (!gaps) return false;
    if (gaps->size()>=64) {
        if (gaps->empty() || gaps->back().kind!=Gap::Kind::limit)
            gaps->push_back(Gap{Gap::Kind::limit,{},0,0,0,0,"additional diagnostic gaps omitted"});
        return true;
    }
    gaps->push_back(std::move(gap));
    return false;
}
SlotScan merge_foreign_slots(const std::vector<SlotScan>& scans,std::size_t skip) {
    SlotScan foreign;
    foreign.module_wild.assign(scans.size(),0);
    for (std::size_t index=0;index<scans.size();++index) {
        if (index==skip) continue;
        const auto& scan=scans[index];
        if (scan.wild_all) foreign.wild_all=true;
        for (std::size_t module=0;module<scan.module_wild.size() && module<foreign.module_wild.size();++module)
            if (scan.module_wild[module]) foreign.module_wild[module]=1;
        for (const auto& [address,effect] : scan.effects) {
            const auto found=foreign.effects.find(address);
            if (found==foreign.effects.end()) { foreign.effects.emplace(address,effect); continue; }
            auto& combined=found->second;
            if (combined.kind==SlotEffect::Kind::poison || effect.kind==SlotEffect::Kind::poison) {
                combined=SlotEffect{}; continue;
            }
            if (combined.kind==SlotEffect::Kind::open || effect.kind==SlotEffect::Kind::open) {
                combined.kind=SlotEffect::Kind::open; combined.codes.clear(); continue;
            }
            combined.codes.insert(effect.codes.begin(),effect.codes.end());
            if (combined.codes.size()>value_limit) { combined.kind=SlotEffect::Kind::open; combined.codes.clear(); }
        }
    }
    return foreign;
}
static bool module_owner(std::span<const elf::IopModule> modules,std::uint32_t address,std::size_t& owner) {
    for (std::size_t index=0;index<modules.size();++index) {
        const auto& module=modules[index];
        const auto extent=module.info ? module.info->text_size : module.initialized_size;
        if (address%4 || address<module.base || address-module.base>=extent) continue;
        owner=index; return true;
    }
    return false;
}
static void replace_callback_roots(std::vector<std::set<std::uint32_t>>& callback_roots,std::vector<std::set<std::uint32_t>>& emitted,
    std::size_t source,const std::set<std::uint32_t>& callbacks,std::span<const elf::IopModule> modules,std::set<std::size_t>& pending) {
    emitted[source]=callbacks;
    std::vector<std::set<std::uint32_t>> next(callback_roots.size());
    for (std::size_t from=0;from<emitted.size();++from)
        for (const auto address : emitted[from]) {
            std::size_t owner=0;
            if (module_owner(modules,address,owner)) next[owner].insert(address);
        }
    for (std::size_t index=0;index<next.size();++index)
        if (next[index]!=callback_roots[index]) {
            callback_roots[index]=std::move(next[index]);
            pending.insert(index);
        }
}
static Program discover_module(const elf::IopModule& module,std::span<const std::uint32_t> entries,std::span<const elf::IopExports> libraries,
    std::span<const elf::IopModule> data_modules,std::set<std::uint32_t>* external_callbacks=nullptr,std::vector<Gap>* gaps=nullptr,
    const SlotScan* foreign_slots=nullptr,SlotScan* slot_scan=nullptr) {
    const auto extent=module.info ? module.info->text_size : module.initialized_size;
    if (module.base%4 || extent%4 || extent>module.memory.size() || extent>module.initialized_size ||
        module.base>=0x200000 || extent>0x200000-module.base)
        throw std::runtime_error("IOP conversion: invalid executable extent");
    Program program; program.entry=module.entry;
    std::map<std::uint32_t,Instruction> native_imports;
    std::ostringstream unresolved;
    for (const auto& import : elf::iop_imports(module)) {
        auto operation=native_operation(import);
        if (import.library=="romdrv" && std::any_of(libraries.begin(),libraries.end(),[&](const auto& library) {
                return library.name==import.library && (library.version>>8)==(import.version>>8);
            })) operation.reset(); // An unreplaced module retains its compiled export ownership.
        const bool host=operation.has_value();
        if (!host) for (const auto& library : libraries)
            if (library.name==import.library && (library.version>>8)==(import.version>>8) && import.ordinal<library.functions.size()) {
                operation=Operation::native_import; break;
            }
        if (!operation) {
            if (gaps) {
                if (note_gap(gaps,Gap{Gap::Kind::import_missing,import.library,import.version,import.ordinal,
                        module.base+import.offset,0,{}})) return program;
                continue;
            }
            unresolved<<"\n  "<<import.library<<" version="<<unsigned(import.version>>8)<<'.'<<unsigned(import.version&255)
                <<" ordinal="<<import.ordinal<<" at "<<hex32(module.base+import.offset);
            continue;
        }
        Instruction call; call.address=module.base+import.offset;
        call.operation=*operation; call.flow=Flow::native_return;
        call.immediate=import.ordinal|(!host ? 0x20000U : import.library=="loadcore" ? 0x10000U : import.library=="ioman" ? 0x30000U : import.library=="intrman" ? 0x40000U : import.library=="thevent" ? 0x50000U : import.library=="thbase" ? 0x60000U : import.library=="sifman" ? 0x70000U : import.library=="stdio" ? 0x80000U : import.library=="sifcmd" ? 0x90000U : import.library=="thsemap" ? 0xa0000U : import.library=="thmsgbx" ? 0xb0000U : import.library=="sysclib" ? 0xc0000U : import.library=="heaplib" ? 0xd0000U : import.library=="modload" ? 0xe0000U : import.library=="romdrv" ? 0xf0000U : import.library=="dmacman" ? 0x100000U : 0U);
        native_imports.emplace(call.address,call);
        if (!host) {
            auto bound=import; bound.offset+=module.base; bound.table_offset+=module.base;
            program.library_imports.push_back(std::move(bound));
        }
    }
    if (!unresolved.str().empty())
        throw std::runtime_error("IOP conversion: unsupported native imports:"+unresolved.str());
    bool halt=false;
    std::set<std::uint32_t> abandoned;
    std::set<std::uint32_t> structural(entries.begin(),entries.end());
    structural.insert(module.entry);
    for (const auto& table : elf::iop_exports(module))
        structural.insert(table.functions.begin(),table.functions.end());
    std::set<std::uint32_t> pending=structural;
    std::map<std::uint32_t,Constants> states;
    std::set<std::uint32_t> table_transfers;
    std::map<std::uint32_t,std::set<std::uint32_t>> stack_transfers;
    std::map<std::uint32_t,std::set<std::uint32_t>> pointer_transfers;
    SlotScan last_scan;
    for (const auto root : pending) states.emplace(root,Constants{});
    const auto enqueue=[&](std::uint32_t target,const Constants& state) {
        const auto [at,inserted]=states.emplace(target,state);
        if (inserted || at->second.merge(state)) pending.insert(target);
    };
    const auto physical_address=[](std::uint32_t address) {
        const auto segment=address&0xe0000000U;
        return (segment==0x80000000U || segment==0xa0000000U) ? address&0x1fffffffU : address;
    };
    // Pointer edges can be retracted when a later store opens the slot.
    // Service, stack, and returned-constant callbacks stay pinned to the
    // instruction that discovered them.
    std::map<std::uint32_t,std::set<std::uint32_t>> pinned_sources,pointer_sources;
    const auto discover_callback=[&](std::uint32_t address,std::uint32_t source,bool from_pointer) {
        const auto target=physical_address(address);
        (from_pointer ? pointer_sources : pinned_sources)[target].insert(source);
        if (external_callbacks && (target<module.base || target-module.base>=extent)) external_callbacks->insert(target);
        else enqueue(target,Constants{});
    };
    const auto discover_stack_thread=[&](std::uint32_t source,const Constants& input) {
        if (!input.stack_offsets[4]) return;
        const auto slot=std::int64_t(*input.stack_offsets[4])+8;
        if (slot>std::numeric_limits<std::int32_t>::max()) return;
        const auto stored=input.stack_words.find(static_cast<std::int32_t>(slot));
        if (stored!=input.stack_words.end() && stored->second)
            for (const auto entry : *stored->second) if (entry) discover_callback(entry,source,false);
    };
    const auto discover_loader_callbacks=[&](std::uint32_t source,std::uint32_t service,const Constants& input) {
        if (service!=0xe000c && service!=0xe000d) return;
        const auto count=service==0xe000c ? 3U : 1U;
        for (unsigned index=0;index<count;++index) if (input.registers[4+index])
            for (const auto target : *input.registers[4+index]) if (target) discover_callback(target,source,false);
    };
    const auto trapped=[&](std::uint32_t pc) {
        Instruction stop; stop.address=pc; stop.flow=Flow::trap; stop.operation=Operation::breakpoint; return stop;
    };
    const auto append=[&](std::uint32_t pc) {
        if (abandoned.contains(pc)) return trapped(pc);
        if (gaps) if (const auto found=program.instructions.find(pc); found!=program.instructions.end()) return found->second;
        if (pc%4 || pc<module.base || pc-module.base>=extent) {
            if (gaps) {
                abandoned.insert(pc);
                if (note_gap(gaps,Gap{Gap::Kind::unresolved_target,{},0,0,pc,0,"unresolved code target"})) halt=true;
                return trapped(pc);
            }
            throw std::runtime_error("IOP conversion: unresolved code target "+hex32(pc));
        }
        if (!program.instructions.contains(pc) && program.instructions.size()>=65536) {
            if (gaps) {
                abandoned.insert(pc);
                if (note_gap(gaps,Gap{Gap::Kind::limit,{},0,0,pc,0,"65536-instruction module analysis limit exceeded"})) halt=true;
                return trapped(pc);
            }
            throw std::runtime_error("IOP conversion: 65536-instruction module analysis limit exceeded");
        }
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
            if (gaps) {
                abandoned.insert(pc);
                std::ostringstream why;
                why<<error.what()<<" primary="<<(word>>26);
                if ((word>>26)==0 || (word>>26)==1) why<<" function="<<(word&63);
                if (note_gap(gaps,Gap{Gap::Kind::instruction,{},0,0,pc,word,why.str()})) halt=true;
                auto stop=trapped(pc);
                program.instructions.emplace(pc,stop);
                return stop;
            }
            throw std::runtime_error("IOP conversion at "+hex32(pc)+" word="+hex32(word)+": "+error.what());
        }
        program.instructions.emplace(pc,instruction);
        program.protected_words.insert(pc);
        return instruction;
    };
    std::set<std::uint32_t> inspected_selectors;
    // Drop callees that were queued by a loaded pointer after that proof is
    // withdrawn. Delay slots stay, but they do not create a fall-through edge.
    // Exports, explicit roots, and callbacks pinned by a reached instruction stay.
    const auto retain_reached=[&]() {
        std::set<std::uint32_t> reachable,queued;
        std::vector<std::uint32_t> work;
        const auto add_control=[&](std::uint32_t next) {
            if (!states.contains(next)) return;
            reachable.insert(next);
            if (queued.insert(next).second) work.push_back(next);
        };
        for (const auto root : structural) add_control(root);
        for (std::size_t cursor=0;cursor<work.size();++cursor) {
            const auto pc=work[cursor];
            const auto found=program.instructions.find(pc);
            if (found==program.instructions.end()) continue;
            const auto& instruction=found->second;
            const auto add_delay=[&]() { if (program.instructions.contains(pc+4)) reachable.insert(pc+4); };
            switch (instruction.flow) {
            case Flow::sequential: case Flow::service: add_control(pc+4); break;
            case Flow::direct: add_delay(); add_control(instruction.target); break;
            case Flow::conditional: add_delay(); add_control(instruction.target); add_control(pc+8); break;
            case Flow::call: add_delay(); add_control(instruction.target); add_control(pc+8); break;
            case Flow::indirect:
                add_delay();
                for (const auto target : instruction.targets) add_control(target);
                break;
            case Flow::indirect_call:
                add_delay();
                for (const auto target : instruction.targets) add_control(target);
                add_control(pc+8);
                break;
            case Flow::return_register: add_delay(); break;
            default: break;
            }
            for (const auto& [target,sources] : pinned_sources) {
                if (!states.contains(target) || queued.contains(target)) continue;
                for (const auto source : sources) if (reachable.contains(source)) { add_control(target); break; }
            }
        }
        if (external_callbacks) {
            std::set<std::uint32_t> kept;
            const auto outside=[&](std::uint32_t target) { return target<module.base || target-module.base>=extent; };
            for (const auto& [target,sources] : pinned_sources) {
                if (!outside(target)) continue;
                for (const auto source : sources) if (reachable.contains(source)) { kept.insert(target); break; }
            }
            for (const auto& [site,targets] : pointer_transfers)
                if (reachable.contains(site)) for (const auto target : targets) {
                    const auto physical=physical_address(target);
                    if (outside(physical)) kept.insert(physical);
                }
            *external_callbacks=std::move(kept);
        }
        for (auto cursor=program.instructions.begin();cursor!=program.instructions.end();) {
            if (reachable.contains(cursor->first)) ++cursor;
            else cursor=program.instructions.erase(cursor);
        }
        std::set<std::uint32_t> kept_words;
        for (const auto& [pc,instruction] : program.instructions) {
            kept_words.insert(pc);
            if (instruction.flow==Flow::native_return) kept_words.insert(pc+4);
        }
        program.protected_words=std::move(kept_words);
        for (auto cursor=program.open_indirect_sites.begin();cursor!=program.open_indirect_sites.end();) {
            if (program.instructions.contains(*cursor)) ++cursor;
            else cursor=program.open_indirect_sites.erase(cursor);
        }
    };
    do {
        while (!pending.empty()) {
            if (halt) break;
            const auto pc=*pending.begin(); pending.erase(pending.begin());
            auto instruction=append(pc);
            if (halt || instruction.flow==Flow::trap) continue;
            const auto input=states.at(pc);
            const auto after=transfer(instruction,input);
            if (instruction.flow==Flow::trap) continue;
            if (instruction.flow==Flow::native_return) {
                if (instruction.operation==Operation::native_import) discover_loader_callbacks(pc,instruction.immediate,input);
                if (instruction.operation==Operation::native_import && instruction.immediate==0x90011 && input.registers[6])
                    for (const auto address : *input.registers[6]) if (address) discover_callback(address,pc,false);
                if (instruction.operation==Operation::native_import && instruction.immediate==0x9000a && input.registers[5])
                    for (const auto address : *input.registers[5]) if (address) discover_callback(address,pc,false);
                if (instruction.operation==Operation::native_import && instruction.immediate==0x40004 && input.registers[6])
                    for (const auto address : *input.registers[6]) discover_callback(address,pc,false);
                if (instruction.operation==Operation::native_import &&
                    (instruction.immediate==0x60023 || instruction.immediate==0x60024) && input.registers[5])
                    for (const auto address : *input.registers[5]) discover_callback(address,pc,false);
                if (instruction.operation==Operation::native_import && instruction.immediate==0x7001e && input.registers[4])
                    for (const auto address : *input.registers[4]) if (address) discover_callback(address,pc,false);
                if (instruction.operation==Operation::native_import && instruction.immediate==0x70020 && input.registers[6])
                    for (const auto address : *input.registers[6]) if (address) discover_callback(address,pc,false);
                if (instruction.operation==Operation::native_import && (instruction.immediate==0xc0012 || instruction.immediate==0x4000e || instruction.immediate==0x10014) && input.registers[4])
                    for (const auto address : *input.registers[4]) if (address) discover_callback(address,pc,false);
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
                        for (const auto offset : {0U,4U,12U,16U,20U,24U,28U,32U,36U,40U,44U,48U,52U,56U}) {
                            const auto function=initial_word(*operations+offset);
                            if (function && *function) discover_callback(*function,pc,false);
                        }
                        const auto type=initial_word(descriptor+4);
                        if (type && (*type&0xf0000000U)==0x10000000U && *operations<=0xffffffffU-104) {
                            for (const auto offset : {68U,72U,76U,80U,84U,88U,92U,96U,100U,104U}) {
                                const auto function=initial_word(*operations+offset);
                                if (function && *function) discover_callback(*function,pc,false);
                            }
                        }
                    }
                }
                if (instruction.operation==Operation::native_import && instruction.immediate==0x60004) discover_stack_thread(pc,input);
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
                            if (entry) discover_callback(entry,pc,false);
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
                else if (const auto table=initial_jump_table(module,data_modules,pc,decode)) {
                    physical=*table; table_transfers.insert(pc);
                }
                else if (const auto callbacks=stack_transfers.find(pc);callbacks!=stack_transfers.end()) physical=callbacks->second;
                else if (const auto loaded=pointer_transfers.find(pc);loaded!=pointer_transfers.end()) physical=loaded->second;
                instruction.targets.assign(physical.begin(),physical.end());
                program.instructions.at(pc).targets=instruction.targets;
            }
            const auto delay=append(pc+4);
            if (halt || delay.flow==Flow::trap) continue;
            if (delay.flow!=Flow::sequential) {
                if (gaps) {
                    if (note_gap(gaps,Gap{Gap::Kind::control,{},0,0,pc+4,0,"branch in delay slot"})) halt=true;
                    continue;
                }
                throw std::runtime_error("IOP conversion: branch in delay slot at "+hex32(pc+4));
            }
            const auto successor=transfer(delay,after);
            // Inspect each direct call before unrelated callers merge at the import.
            if (instruction.flow==Flow::call || instruction.flow==Flow::direct) {
                const auto target=native_imports.find(instruction.target);
                if (target!=native_imports.end() && target->second.immediate==0x60004) discover_stack_thread(pc,successor);
                if (target!=native_imports.end()) discover_loader_callbacks(pc,target->second.immediate,successor);
            }
            if (indirect) {
                // Loaded-pointer callees start with unknown registers. The caller's
                // post-delay constants are not a proof about the callee.
                const bool loaded_pointer=pointer_transfers.contains(pc);
                for (const auto target : instruction.targets) enqueue(target,loaded_pointer ? Constants{} : successor);
            } else if (instruction.flow!=Flow::return_register) enqueue(instruction.target,successor);
            if (instruction.flow==Flow::conditional) enqueue(pc+8,successor);
            if (instruction.flow==Flow::call || instruction.flow==Flow::indirect_call) enqueue(pc+8,Constants{});
        }
        // A helper can return a callback which its caller stores in a thread
        // descriptor. The ordinary call-clobber analysis intentionally loses v0;
        // retain all bounded constant return candidates as additional AOT roots.
        std::set<std::uint32_t> selectors;
        for (const auto& [address,instruction] : program.instructions) {
            if (instruction.flow==Flow::call) selectors.insert(instruction.target);
            if (instruction.flow==Flow::indirect_call)
                selectors.insert(instruction.targets.begin(),instruction.targets.end());
        }
        for (const auto selector : selectors) {
            if (!inspected_selectors.insert(selector).second) continue;
            for (const auto value : returned_constants(program,selector)) {
                const auto target=physical_address(value);
                if (target%4 || program.instructions.contains(target)) continue;
                for (const auto& owner : data_modules) {
                    const auto text=owner.info ? owner.info->text_size : owner.initialized_size;
                    if (target>=owner.base && target-owner.base<text) { discover_callback(target,selector,false); break; }
                }
            }
        }
        const auto candidates=stored_code_candidates(program,states,data_modules);
        if (!candidates.empty()) for (const auto& [pc,instruction] : program.instructions) {
            if (states.contains(pc) && !states.at(pc).registers[instruction.source] && stack_callback_call(program,instruction)) {
                auto& targets=stack_transfers[pc];
                const auto before=targets.size(); targets.insert(candidates.begin(),candidates.end());
                if (targets.size()>256) {
                    if (gaps) {
                        if (note_gap(gaps,Gap{Gap::Kind::limit,{},0,0,pc,0,"callback candidate limit exceeded"})) halt=true;
                        break;
                    }
                    throw std::runtime_error("IOP conversion: callback candidate limit exceeded at "+hex32(pc));
                }
                if (targets.size()!=before) pending.insert(pc);
            }
        }
        if (!halt) {
            const auto recovered=recoverable_loaded_pointers(program,states,data_modules,foreign_slots);
            last_scan=recovered.scan;
            if (slot_scan) *slot_scan=last_scan;
            std::vector<std::uint32_t> stale;
            for (const auto& [site,targets] : pointer_transfers)
                if (const auto found=recovered.proved.find(site); found==recovered.proved.end() || found->second!=targets) stale.push_back(site);
            for (const auto site : stale) {
                pointer_transfers.erase(site);
                if (const auto found=program.instructions.find(site); found!=program.instructions.end()) found->second.targets.clear();
            }
            for (const auto& [site,targets] : recovered.proved) {
                if (const auto found=pointer_transfers.find(site); found!=pointer_transfers.end() && found->second==targets) continue;
                pointer_transfers[site]=targets;
                auto& recovered_instruction=program.instructions.at(site);
                recovered_instruction.targets.assign(targets.begin(),targets.end());
                for (const auto target : targets) discover_callback(target,site,true);
            }
            // An unbounded store through one fixed slot is not a 16-value proof
            // and must not enqueue callees from that edge.
            std::set<std::uint32_t> open_sites;
            for (const auto site : recovered.open)
                if (!table_transfers.contains(site) && !stack_transfers.contains(site)) open_sites.insert(site);
            for (const auto site : open_sites)
                if (const auto found=program.instructions.find(site); found!=program.instructions.end()) found->second.targets.clear();
            program.open_indirect_sites=std::move(open_sites);
        }
        if (halt) break;
    } while (!pending.empty());
    if (!halt) retain_reached();
    if (halt) return program;
    const auto unresolved_indirect=[&](std::uint32_t pc,const Instruction& i) {
        return (i.flow==Flow::indirect || i.flow==Flow::indirect_call) && !states.at(pc).registers[i.source] &&
            !table_transfers.contains(pc) && !stack_transfers.contains(pc) && !pointer_transfers.contains(pc) &&
            !program.open_indirect_sites.contains(pc);
    };
    std::ostringstream indirect_errors;
    for (const auto& [pc,i] : program.instructions)
        if (unresolved_indirect(pc,i))
            indirect_errors<<"\n  unresolved indirect transfer at "<<hex32(pc)<<" through r"<<i.source;
    if (!indirect_errors.str().empty()) {
        if (gaps) {
            for (const auto& [pc,i] : program.instructions)
                if (unresolved_indirect(pc,i))
                    if (note_gap(gaps,Gap{Gap::Kind::indirect,{},0,0,pc,0,"r"+std::to_string(i.source)})) break;
        } else throw std::runtime_error("IOP conversion: indirect target sets are unknown or exceed the 16-value analysis bound:"+indirect_errors.str());
    }
    for (const auto& [pc,targets] : stack_transfers)
        if (program.instructions.contains(pc)) program.guarded_callback_sites.insert(pc);
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
    std::vector<std::set<std::uint32_t>> entry_roots(modules.size()), callback_roots(modules.size()), emitted(modules.size());
    std::set<std::size_t> pending;
    for (std::size_t index=0;index<modules.size();++index) pending.insert(index);
    const auto add_entry=[&](std::uint32_t address) {
        std::size_t owner=0;
        if (!module_owner(modules,address,owner))
            throw std::runtime_error("IOP conversion: bundle code root has no executable owner at "+hex32(address));
        entry_roots[owner].insert(address);
        pending.insert(owner);
    };
    const auto module_roots_of=[&](std::size_t index) {
        std::set<std::uint32_t> combined=entry_roots[index];
        combined.insert(callback_roots[index].begin(),callback_roots[index].end());
        return std::vector<std::uint32_t>(combined.begin(),combined.end());
    };
    const auto publish_callbacks=[&](std::size_t source,const std::set<std::uint32_t>& callbacks) {
        for (const auto address : callbacks) {
            std::size_t owner=0;
            if (!module_owner(modules,address,owner))
                throw std::runtime_error("IOP conversion: bundle code root has no executable owner at "+hex32(address));
        }
        replace_callback_roots(callback_roots,emitted,source,callbacks,modules,pending);
    };
    for (const auto entry : entries) add_entry(entry);
    // Callback roots follow the latest finished analysis. A retracted loaded
    // pointer drops the callee and revisits its former owner.
    std::vector<Program> programs(modules.size());
    std::vector<std::string> failures(modules.size());
    std::vector<SlotScan> scans(modules.size()), applied(modules.size());
    unsigned slot_rounds=0, steps=0;
    for (;;) {
    while (!pending.empty()) {
        if (++steps>4096) throw std::runtime_error("IOP conversion: module reanalysis step limit exceeded");
        const auto index=*pending.begin(); pending.erase(pending.begin());
        const auto module_roots=module_roots_of(index);
        std::set<std::uint32_t> callbacks;
        applied[index]=merge_foreign_slots(scans,index);
        try {
            programs[index]=discover_module(modules[index],module_roots,libraries,modules,&callbacks,nullptr,&applied[index],&scans[index]);
            failures[index].clear();
        } catch (const std::runtime_error& error) {
            // Analyze the other owners as well: a reboot archive is one native
            // program, and its conversion diagnostics must expose the whole
            // missing operation/service batch before any output is emitted.
            failures[index]=error.what();
        }
        publish_callbacks(index,callbacks);
    }
    bool failed=false;
    for (const auto& item : failures) if (!item.empty()) failed=true;
    if (failed) break;
    bool again=false;
    for (std::size_t index=0;index<modules.size();++index)
        if (merge_foreign_slots(scans,index)!=applied[index]) { pending.insert(index); again=true; }
    if (!again) break;
    if (++slot_rounds>32) throw std::runtime_error("IOP conversion: bundle slot analysis did not converge");
    }
    std::ostringstream diagnostics;
    for (std::size_t index=0;index<modules.size();++index) if (!failures[index].empty()) {
        diagnostics<<"\nmodule "<<index<<" base="<<hex32(modules[index].base);
        if (modules[index].info) diagnostics<<" name="<<modules[index].info->name;
        diagnostics<<": "<<failures[index]<<'\n';
    }
    if (!diagnostics.str().empty())
        throw std::runtime_error("IOP bundle conversion failed:"+diagnostics.str());
    Program result; result.entry=modules.front().entry;
    for (const auto& program : programs) {
        result.instructions.insert(program.instructions.begin(),program.instructions.end());
        result.protected_words.insert(program.protected_words.begin(),program.protected_words.end());
        result.library_imports.insert(result.library_imports.end(),program.library_imports.begin(),program.library_imports.end());
        result.guarded_callback_sites.insert(program.guarded_callback_sites.begin(),program.guarded_callback_sites.end());
        result.open_indirect_sites.insert(program.open_indirect_sites.begin(),program.open_indirect_sites.end());
    }
    return result;
}
std::vector<ModuleDiagnosis> diagnose_bundle(std::span<const elf::IopModule> modules,std::span<const std::uint32_t> entries) {
    if (modules.empty()) throw std::runtime_error("IOP conversion: empty module bundle");
    std::vector<ModuleDiagnosis> results(modules.size());
    std::vector<elf::IopExports> libraries;
    std::vector<std::string> blocked(modules.size());
    for (std::size_t index=0;index<modules.size();++index) {
        const auto& module=modules[index];
        const auto end=std::uint64_t(module.base)+module.memory.size();
        if (end>0x200000) throw std::runtime_error("IOP conversion: bundle module exceeds data arena");
        for (std::size_t other=0;other<index;++other)
            if (module.base<std::uint64_t(modules[other].base)+modules[other].memory.size() && modules[other].base<end)
                throw std::runtime_error("IOP conversion: overlapping bundle modules");
        try {
            auto exports=elf::iop_exports(module);
            libraries.insert(libraries.end(),exports.begin(),exports.end());
        } catch (const std::exception& error) { blocked[index]=error.what(); }
    }
    std::vector<std::set<std::uint32_t>> entry_roots(modules.size()), callback_roots(modules.size()), emitted(modules.size());
    std::set<std::size_t> pending;
    for (std::size_t index=0;index<modules.size();++index) {
        if (blocked[index].empty()) pending.insert(index);
        else results[index].unexpected=blocked[index];
    }
    const auto add_entry=[&](std::uint32_t address) {
        std::size_t owner=0;
        if (!module_owner(modules,address,owner)) return false;
        entry_roots[owner].insert(address);
        pending.insert(owner);
        return true;
    };
    const auto module_roots_of=[&](std::size_t index) {
        std::set<std::uint32_t> combined=entry_roots[index];
        combined.insert(callback_roots[index].begin(),callback_roots[index].end());
        return std::vector<std::uint32_t>(combined.begin(),combined.end());
    };
    for (const auto entry : entries) if (!add_entry(entry))
        results.front().gaps.push_back(Gap{Gap::Kind::unresolved_target,{},0,0,entry,0,"bundle code root has no executable owner"});
    unsigned steps=0;
    std::vector<SlotScan> scans(modules.size()), applied(modules.size());
    unsigned slot_rounds=0;
    bool limited=false;
    for (;;) {
    while (!pending.empty()) {
        if (++steps>4096) {
            results.front().gaps.push_back(Gap{Gap::Kind::limit,{},0,0,0,0,"module reanalysis step limit exceeded"});
            limited=true;
            break;
        }
        const auto index=*pending.begin(); pending.erase(pending.begin());
        if (!results[index].unexpected.empty()) continue;
        const auto module_roots=module_roots_of(index);
        std::set<std::uint32_t> callbacks;
        std::vector<Gap> gaps;
        applied[index]=merge_foreign_slots(scans,index);
        try {
            results[index].program=discover_module(modules[index],module_roots,libraries,modules,&callbacks,&gaps,&applied[index],&scans[index]);
            results[index].gaps=std::move(gaps);
        } catch (const std::exception& error) {
            results[index].unexpected=error.what();
            results[index].gaps=std::move(gaps);
        }
        if (!results[index].unexpected.empty()) continue;
        std::set<std::uint32_t> owned;
        for (const auto callback : callbacks) {
            std::size_t owner=0;
            if (!module_owner(modules,callback,owner)) {
                bool seen=false;
                for (const auto& existing : results[index].gaps)
                    if (existing.kind==Gap::Kind::unresolved_target && existing.address==callback && existing.detail=="callback has no executable owner")
                        seen=true;
                if (!seen) results[index].gaps.push_back(Gap{Gap::Kind::unresolved_target,{},0,0,callback,0,"callback has no executable owner"});
                continue;
            }
            owned.insert(callback);
        }
        replace_callback_roots(callback_roots,emitted,index,owned,modules,pending);
    }
    if (limited) break;
    bool diagnosis_stopped=false;
    for (const auto& item : results) if (!item.unexpected.empty()) diagnosis_stopped=true;
    if (diagnosis_stopped) break;
    bool again=false;
    for (std::size_t index=0;index<modules.size();++index)
        if (merge_foreign_slots(scans,index)!=applied[index]) { pending.insert(index); again=true; }
    if (!again) break;
    if (++slot_rounds>32) {
        results.front().gaps.push_back(Gap{Gap::Kind::limit,{},0,0,0,0,"bundle slot analysis did not converge"});
        break;
    }
    }
    return results;
}
}
