#include <nt/analysis.hpp>
#include <array>
#include <algorithm>
#include <deque>
#include <optional>
#include <stdexcept>

namespace nt::analysis {
std::set<std::uint32_t> syscall_wrappers(ByteView bytes,const elf::Executable& executable) {
    std::set<std::uint32_t> result;
    for (const auto& segment:executable.segments) {
        if (!(segment.flags&1)) continue;
        if (std::uint64_t(segment.offset)+segment.file_size>bytes.size() ||
            std::uint64_t(segment.address)+segment.file_size>0x100000000ULL)
            throw std::invalid_argument("Invalid syscall-wrapper scan extent");
        const auto first=(4U-(segment.address&3U))&3U;
        for (std::uint64_t offset=first;offset+16<=segment.file_size;offset+=4) {
            const auto pc=segment.address+static_cast<std::uint32_t>(offset);
            const auto at=static_cast<std::size_t>(segment.offset+offset);
            const auto setup=r5900::decode(pc,le32(bytes,at));
            if (setup.rs || setup.rt!=3 ||
                (setup.op!=r5900::Op::addiu && setup.op!=r5900::Op::daddiu && setup.op!=r5900::Op::ori)) continue;
            if (r5900::decode(pc+4,le32(bytes,at+4)).op!=r5900::Op::syscall ||
                le32(bytes,at+8)!=0x03e00008U || le32(bytes,at+12)!=0) continue;
            result.insert(pc);
        }
    }
    return result;
}

Program discover(ByteView bytes, const elf::Executable& elf, std::size_t limit,
                 std::span<const std::uint32_t> additional_entries, bool stop_at_syscall) {
    const auto word_at = [&](std::uint32_t pc) -> std::optional<std::uint32_t> {
        if (pc % 4) return {};
        for (const auto& s : elf.segments)
            if ((s.flags & 1) && pc >= s.address && std::uint64_t(pc) + 4 <= std::uint64_t(s.address) + s.file_size)
                return le32(bytes, std::size_t(s.offset) + pc - s.address);
        return {};
    };
    Program result;
    // A delay slot is visited as an instruction but must not independently cause
    // fallthrough from an unconditional jump. A separate ordinary visit can.
    std::deque<std::pair<std::uint32_t, bool>> pending{{elf.entry, false}};
    for (const auto pc:elf.lifecycle_entries) pending.emplace_back(pc,false);
    for (auto pc : additional_entries) {
        if (!word_at(pc)) throw std::runtime_error("Additional entry is not executable ELF memory");
        pending.emplace_back(pc,false);
    }
    std::set<std::pair<std::uint32_t, bool>> visited;
    while (!pending.empty()) {
        const auto [pc, delay] = pending.front(); pending.pop_front();
        if (!visited.emplace(pc, delay).second) continue;
        const auto word = word_at(pc);
        if (!word) { result.unresolved.insert(pc); continue; }
        const auto instruction = r5900::decode(pc, *word);
        result.instructions.emplace(pc, instruction);
        if (result.instructions.size() > limit) throw std::runtime_error("Analysis instruction limit exceeded");
        if (delay || instruction.terminal()) continue;
        if (stop_at_syscall && instruction.op==r5900::Op::syscall) {
            // A leaf syscall wrapper can return through a dynamically registered
            // compiled handler even when its service number is not known here.
            // Discover its return epilogue without assuming syscall success.
            if (pc<=0xfffffff7U) {
                const auto tail=word_at(pc+4),slot=word_at(pc+8);
                if (tail && slot && *slot==0) {
                    const auto next=r5900::decode(pc+4,*tail);
                    if (next.op==r5900::Op::jr && next.rs==31) pending.emplace_back(pc+4,false);
                }
            }
            continue;
        }
        if (instruction.branch()) {
            pending.emplace_back(pc + 4, true);
            if (!instruction.indirect()) pending.emplace_back(instruction.target(), false);
            if (instruction.op != r5900::Op::j && instruction.op != r5900::Op::jr)
                pending.emplace_back(pc + 8, false);
        } else {
            // A syscall is a runtime boundary, but analysis may inspect its continuation.
            pending.emplace_back(pc + 4, false);
        }
    }
    return result;
}
namespace {
std::optional<std::uint32_t> initial_word(ByteView bytes,const elf::Executable& elf,std::uint32_t address) {
    if (address>=0x80000000U && address<0xc0000000U) address&=0x1fffffffU;
    if (address%4) return {};
    for (const auto& segment:elf.segments)
        if (address>=segment.address && std::uint64_t(address)+4<=std::uint64_t(segment.address)+segment.file_size)
            return le32(bytes,std::size_t(segment.offset)+address-segment.address);
    return {};
}
std::optional<std::uint32_t> executable_pointer(const elf::Executable& elf,std::uint32_t value) {
    if (value>=0x80000000U && value<0xc0000000U) value&=0x1fffffffU;
    if (!value || value%4) return {};
    for (const auto& segment:elf.segments)
        if ((segment.flags&1) && value>=segment.address &&
            std::uint64_t(value)+4<=std::uint64_t(segment.address)+segment.file_size) return value;
    return {};
}
// Track exact argument copies, not general data dependencies. A bounded callee
// summary identifies arguments written to word-sized callback/data slots.
unsigned stored_arguments(const Program& program,std::uint32_t entry) {
    using r5900::Op;
    using Origins=std::array<unsigned,32>;
    Origins initial{};
    // The EE ABI passes eight integer arguments in registers a0 through t3.
    for (unsigned reg=4;reg<12;++reg) initial[reg]=1U<<(reg-4);
    std::deque<std::pair<std::uint32_t,Origins>> pending{{entry,initial}};
    std::set<std::pair<std::uint32_t,Origins>> visited;
    unsigned stored=0,budget=256;
    const auto transfer=[&](const r5900::Instruction& i,Origins& origins) {
        unsigned destination=0,value=0;
        switch (i.op) {
        case Op::sw: stored|=origins[i.rt]; return true;
        case Op::sb: case Op::sh: case Op::sd: case Op::sq: return true;
        case Op::addu: case Op::daddu: case Op::bit_or:
            destination=i.rd;
            if (!i.rt) value=origins[i.rs];
            else if (!i.rs) value=origins[i.rt];
            break;
        case Op::addiu: case Op::daddiu: case Op::ori:
            destination=i.rt; if (!i.immediate) value=origins[i.rs]; break;
        case Op::sll: destination=i.rd; if (!i.sa) value=origins[i.rt]; break;
        case Op::lui: case Op::lw: case Op::lwu: case Op::ld: case Op::lq:
        case Op::lb: case Op::lbu: case Op::lh: case Op::lhu:
        case Op::andi: case Op::xori: case Op::slti: case Op::sltiu:
            destination=i.rt; break;
        case Op::bit_and: case Op::bit_xor: case Op::bit_nor: case Op::subu:
        case Op::dsubu: case Op::slt: case Op::sltu: case Op::srl: case Op::sra:
        case Op::sllv: case Op::srlv: case Op::srav: case Op::mult: case Op::multu:
        case Op::mflo: case Op::mfhi:
            destination=i.rd; break;
        default: return false;
        }
        if (destination) origins[destination]=value;
        return true;
    };
    while (!pending.empty() && budget--) {
        auto [pc,origins]=pending.front(); pending.pop_front();
        if (!visited.emplace(pc,origins).second || pc>0xfffffff7U) continue;
        const auto found=program.instructions.find(pc);
        if (found==program.instructions.end()) continue;
        const auto& i=found->second;
        if (!i.branch()) {
            if (transfer(i,origins)) pending.emplace_back(pc+4,origins);
            continue;
        }
        // ABI-preserved copies may reach registration after a helper call.
        // This only proposes compilation roots; it never folds runtime values.
        if (i.op==Op::jal || i.op==Op::jalr) {
            origins[i.op==Op::jal ? 31 : i.rd]=0;
            const auto slot=program.instructions.find(pc+4);
            if (slot==program.instructions.end() || !transfer(slot->second,origins)) continue;
            for (unsigned reg=1;reg<32;++reg)
                if (!(reg>=16 && reg<=23) && reg!=28 && reg!=29 && reg!=30) origins[reg]=0;
            pending.emplace_back(pc+8,origins);
            continue;
        }
        if (i.op==Op::bltzal || i.op==Op::bgezal ||
            i.op==Op::bltzall || i.op==Op::bgezall) continue;
        const auto before_slot=origins;
        const auto slot=program.instructions.find(pc+4);
        if (slot==program.instructions.end() || !transfer(slot->second,origins)) continue;
        if (!i.indirect()) pending.emplace_back(i.target(),origins);
        if (i.op!=Op::j && i.op!=Op::jr) pending.emplace_back(pc+8,i.likely() ? before_slot : origins);
    }
    return stored;
}
// Bounded backward slicing is used only to propose code for compilation. A load
// reads the initial ELF image, not a claim that writable data stays constant.
std::optional<std::uint32_t> candidate(ByteView bytes,const elf::Executable& elf,const Program& program,
                                     std::uint32_t before,unsigned reg,unsigned& fuel,std::uint32_t call_site,bool layout_candidate=false) {
    using r5900::Op;
    if (!reg) return 0;
    while (before>=4 && fuel) {
        --fuel; before-=4;
        const auto found=program.instructions.find(before);
        if (found==program.instructions.end()) return {};
        const auto& i=found->second;
        if (i.op==Op::jal) {
            if (before==call_site && reg!=31) continue;
            // Saved registers are ABI candidates across calls, never proven values.
            if (reg>=16 && reg<=23) continue;
            return {};
        }
        if (i.branch() || i.terminal() || i.op==Op::syscall) {
            if (layout_candidate && reg>=16 && reg<=23) continue;
            return {};
        }
        if (layout_candidate && (i.op==Op::mtc0 || i.op==Op::sync)) continue;
        unsigned destination=0;
        switch (i.op) {
        case Op::sw: case Op::sd: case Op::sq: case Op::sb: case Op::sh: continue;
        case Op::slti: case Op::sltiu:
        case Op::lui: case Op::addiu: case Op::daddiu: case Op::ori: case Op::lw: case Op::lwu:
            destination=i.rt; break;
        case Op::slt: case Op::sltu:
        case Op::addu: case Op::daddu: case Op::bit_or: case Op::sll:
            destination=i.rd; break;
        default: return {};
        }
        if (destination!=reg) continue;
        if (i.op==Op::slti || i.op==Op::sltiu || i.op==Op::slt || i.op==Op::sltu) return {};
        if (i.op==Op::lui) return static_cast<std::uint32_t>(i.immediate)<<16;
        const auto a=candidate(bytes,elf,program,before,i.rs,fuel,call_site,layout_candidate);
        if (!a) return {};
        if (i.op==Op::addiu || i.op==Op::daddiu) return *a+static_cast<std::uint32_t>(i.immediate);
        if (i.op==Op::ori) return *a|static_cast<std::uint16_t>(i.immediate);
        if (i.op==Op::lw || i.op==Op::lwu) return initial_word(bytes,elf,*a+static_cast<std::uint32_t>(i.immediate));
        const auto b=candidate(bytes,elf,program,before,i.rt,fuel,call_site,layout_candidate);
        if (!b) return {};
        if (i.op==Op::sll) return *b<<i.sa;
        return i.op==Op::bit_or ? *a|*b : *a+*b;
    }
    return {};
}
}
std::vector<LayoutCandidate> layout_candidates(ByteView bytes,const elf::Executable& elf,const Program& program) {
    using r5900::Op;
    std::vector<LayoutCandidate> result;
    std::set<std::uint32_t> sources;
    for (const auto& [pc,call]:program.instructions) {
        if (call.op!=Op::jal || pc<12) continue;
        const auto setup=program.instructions.find(call.target()),trap=program.instructions.find(call.target()+4);
        if (setup==program.instructions.end() || trap==program.instructions.end()) continue;
        const auto& service=setup->second;
        if (service.op!=Op::addiu || service.rs || service.rt!=3 || service.immediate!=0x56 || trap->second.op!=Op::syscall) continue;
        unsigned base=0; bool matches=true;
        for (unsigned n=0;n<3;++n) {
            const auto load=program.instructions.find(pc-12+n*4);
            if (load==program.instructions.end() || load->second.op!=Op::lw ||
                load->second.rt!=6+n || load->second.immediate!=static_cast<std::int32_t>(4+4*n)) { matches=false; break; }
            if (n && base!=load->second.rs) { matches=false; break; }
            base=load->second.rs;
        }
        if (!matches || !base) continue;
        unsigned fuel=512;
        const auto pointer=candidate(bytes,elf,program,pc,base,fuel,pc,true);
        if (!pointer || (*pointer&3)) continue;
        for (unsigned row=0;row<48;++row) {
            const auto at=std::uint64_t(*pointer)+16*row;
            if (at+16>0x100000000ULL) break;
            LayoutCandidate record{static_cast<std::uint32_t>(at),pc,{}};
            bool complete=true;
            for (unsigned n=0;n<4;++n) {
                const auto value=initial_word(bytes,elf,record.source+4*n);
                if (!value) { complete=false; break; }
                record.fields[n]=*value;
            }
            if (!complete) break;
            const auto [mask,address,lo0,lo1]=record.fields;
            bool valid_mask=false;
            for (auto value:{0U,0x6000U,0x1e000U,0x7e000U,0x1fe000U,0x7fe000U,0x1ffe000U}) valid_mask|=mask==value;
            if (!valid_mask || (address&255) || (lo0&0x7c000000U) || (lo1&0xfc000000U)) break;
            if (!(lo0&0x80000000U) && (address&(mask|0x1fffU))) break;
            if (sources.insert(record.source).second) result.push_back(record);
        }
    }
    return result;
}
std::set<std::uint32_t> syscall_callbacks(ByteView bytes,const elf::Executable& elf,
                                        const Program& program,std::uint32_t syscall,std::set<std::uint32_t>* registered_calls) {
    using r5900::Op;
    std::set<std::uint32_t> result;
    for (const auto& [pc,call]:program.instructions) {
        if (call.op!=Op::jal || pc>0xfffffff7U) continue;
        const auto setup=program.instructions.find(call.target());
        const auto boundary=program.instructions.find(call.target()+4);
        if (setup==program.instructions.end() || boundary==program.instructions.end()) continue;
        const auto& i=setup->second;
        if (i.rs || i.rt!=3 || (i.op!=Op::addiu && i.op!=Op::daddiu && i.op!=Op::ori) ||
            static_cast<std::uint32_t>(i.immediate)!=syscall || boundary->second.op!=Op::syscall) continue;
        // Include the call's delay slot when recovering argument a1.
        unsigned fuel=128;
        const auto target=candidate(bytes,elf,program,pc+8,5,fuel,pc);
        if (!target || !*target) continue;
        auto address=*target;
        if (address>=0x80000000U && address<0xc0000000U) address&=0x1fffffffU;
        if (address%4) continue;
        for (const auto& segment:elf.segments)
            if ((segment.flags&1) && address>=segment.address &&
                std::uint64_t(address)+4<=std::uint64_t(segment.address)+segment.file_size)
            {
                result.insert(address);
                if (registered_calls) {
                    unsigned number_fuel=128;
                    const auto number=candidate(bytes,elf,program,pc+8,4,number_fuel,pc);
                    if (number && *number<256) registered_calls->insert(*number);
                }
            }
    }
    return result;
}
std::set<std::uint32_t> materialized_code_pointers(ByteView bytes,const elf::Executable& elf,const Program& program) {
    using r5900::Op;
    std::set<std::uint32_t> result;
    for (const auto& [pc,i]:program.instructions) {
        unsigned reg=0;
        switch (i.op) {
        case Op::lui: case Op::ori: case Op::addiu: case Op::daddiu: case Op::lw: case Op::lwu: reg=i.rt; break;
        case Op::addu: case Op::daddu: case Op::bit_or: reg=i.rd; break;
        default: continue;
        }
        // Returned pointer candidates only; argument/data-address materialization
        // alone is not evidence of an executable entry.
        if (reg!=2 || pc>0xfffffffbU) continue;
        unsigned fuel=128;
        const auto value=candidate(bytes,elf,program,pc+4,reg,fuel,0xffffffffU);
        if (!value) continue;
        if (const auto address=executable_pointer(elf,*value)) result.insert(*address);
    }
    return result;
}
// Stores of recoverable executable addresses provide candidates for callback
// tables built during startup. Do not fold the stored value or constrain later
// indirect dispatch: writable tables may change after initialization.
std::set<std::uint32_t> stored_code_pointers(ByteView bytes,const elf::Executable& elf,const Program& program) {
    std::set<std::uint32_t> result;
    for (const auto& [pc,i]:program.instructions) {
        if (i.op!=r5900::Op::sw || !i.rt) continue;
        unsigned fuel=128;
        // A call delay-slot store consumes the value before the callee runs.
        const auto prior=pc>=4 ? program.instructions.find(pc-4) : program.instructions.end();
        const auto call_site=prior!=program.instructions.end() && prior->second.op==r5900::Op::jal ? pc-4 : 0xffffffffU;
        const auto value=candidate(bytes,elf,program,pc,i.rt,fuel,call_site);
        if (!value || !*value) continue;
        if (const auto address=executable_pointer(elf,*value)) result.insert(*address);
    }
    return result;
}
std::set<std::uint32_t> bounded_switch_entries(ByteView bytes,const elf::Executable& elf,const Program& program) {
    using r5900::Op;
    std::set<std::uint32_t> result;
    for (const auto& [pc,jump]:program.instructions) {
        if (jump.op!=Op::jr || !jump.rs || pc<28) continue;
        std::array<const r5900::Instruction*,6> prior{};
        bool complete=true;
        for (unsigned n=0;n<prior.size();++n) {
            const auto found=program.instructions.find(pc-4*(n+1));
            if (found==program.instructions.end()) { complete=false; break; }
            prior[n]=&found->second;
        }
        if (!complete) continue;
        const auto& load=*prior[0]; const auto& add=*prior[1];
        const bool separate_low=prior[2]->op==Op::addiu;
        const auto& high=*(separate_low ? prior[4] : prior[2]);
        const auto& scale=*prior[3];
        const auto* branch_instruction=prior[5];
        if (separate_low && branch_instruction->op!=Op::beq) {
            // The table high half may follow an independent branch-delay move.
            // Reject delay slots that change the index before scaling it.
            const auto& delay=*prior[5];
            const bool move=(delay.op==Op::addu || delay.op==Op::daddu || delay.op==Op::bit_or) && (!delay.rs || !delay.rt);
            const bool nop=delay.op==Op::sll && !delay.rd;
            const auto earlier=program.instructions.find(pc-28);
            if ((!move && !nop) || (delay.rd && delay.rd==scale.rt) || earlier==program.instructions.end()) continue;
            branch_instruction=&earlier->second;
        }
        const auto& branch=*branch_instruction;
        if (separate_low && (prior[2]->rs!=high.rt || prior[2]->rt!=high.rt || high.rt==scale.rt)) continue;
        if (branch.op!=Op::beq || (branch.rs && branch.rt) || (!branch.rs && !branch.rt)) continue;
        const auto guard=branch.rs ? branch.rs : branch.rt;
        // A compiler can schedule register setup and stack saves between the
        // range comparison and its branch. Search a bounded straight-line
        // suffix, rejecting writes to either the guard or switch index.
        const r5900::Instruction* comparison=nullptr;
        for (unsigned distance=4;distance<=64 && distance<=branch.pc;distance+=4) {
            const auto found=program.instructions.find(branch.pc-distance);
            if (found==program.instructions.end()) break;
            const auto& instruction=found->second;
            if (instruction.op==Op::sltiu && instruction.rt==guard) { comparison=&instruction; break; }
            unsigned destination=0;
            switch (instruction.op) {
            case Op::sb: case Op::sh: case Op::sw: case Op::sd: case Op::sq: break;
            case Op::lui: case Op::ori: case Op::andi: case Op::xori: case Op::addiu: case Op::daddiu:
                destination=instruction.rt; break;
            case Op::addu: case Op::daddu: case Op::subu: case Op::dsubu:
            case Op::bit_or: case Op::bit_and: case Op::bit_xor: case Op::sll: case Op::srl: case Op::sra:
                destination=instruction.rd; break;
            default: destination=guard; break;
            }
            if (destination && (destination==guard || destination==scale.rt)) break;
        }
        if (!comparison) continue;
        const auto& bound=*comparison;
        if (load.op!=Op::lw || load.rt!=jump.rs || add.op!=Op::addu || add.rd!=load.rs ||
            high.op!=Op::lui || !high.rt || scale.op!=Op::sll || scale.sa!=2 || !scale.rd ||
            high.rt==scale.rd ||
            !((add.rs==high.rt && add.rt==scale.rd) || (add.rt==high.rt && add.rs==scale.rd)) ||
            bound.op!=Op::sltiu || bound.rs!=scale.rt || !bound.rt ||
            bound.immediate<=0 || bound.immediate>4096 || branch.op!=Op::beq ||
            !((branch.rs==bound.rt && !branch.rt) || (branch.rt==bound.rt && !branch.rs)) ||
            branch.target()<=pc) continue;
        // Candidate entries only: this is not a proof used to remove the guard
        // or to fold table contents. The compiled code retains both at runtime.
        const auto base=(std::uint32_t(high.immediate)<<16)+std::uint32_t(load.immediate)+
            (separate_low ? std::uint32_t(prior[2]->immediate) : 0U);
        std::set<std::uint32_t> entries;
        for (unsigned n=0;n<static_cast<unsigned>(bound.immediate);++n) {
            if (std::uint64_t(base)+4*n>0xffffffffULL) { entries.clear(); break; }
            const auto word=initial_word(bytes,elf,base+4*n);
            const auto entry=word ? executable_pointer(elf,*word) : std::nullopt;
            if (!entry) { entries.clear(); break; }
            entries.insert(*entry);
        }
        result.insert(entries.begin(),entries.end());
    }
    return result;
}
std::set<std::uint32_t> object_table_entries(ByteView bytes,const elf::Executable& elf,const Program& program) {
    using r5900::Op;
    std::set<std::pair<std::int32_t,std::uint32_t>> slots;
    for (const auto& [pc,load]:program.instructions) {
        if (load.op!=Op::lw || !load.rt) continue;
        std::uint32_t offset=0;
        for (unsigned step=1;step<=16 && std::uint64_t(pc)+step*4+4<=0xffffffffULL;++step) {
            const auto found=program.instructions.find(pc+step*4);
            if (found==program.instructions.end()) break;
            const auto& i=found->second;
            if (i.op==Op::addiu && i.rt==load.rt && i.rs==load.rt) {
                offset+=std::uint32_t(i.immediate); continue;
            }
            if (i.op==Op::lw && i.rs==load.rt && i.rt) {
                const auto call=program.instructions.find(i.pc+4);
                if (call!=program.instructions.end() && call->second.op==Op::jalr && call->second.rs==i.rt) {
                    slots.emplace(load.immediate,offset+std::uint32_t(i.immediate));
                    break;
                }
            }
            if (i.op==Op::sw || i.op==Op::sd || i.op==Op::sq || i.op==Op::sb || i.op==Op::sh) continue;
            // Argument loads can be scheduled between the table pointer and
            // its slot load. They do not invalidate a distinct base register.
            if ((i.op==Op::lb || i.op==Op::lbu || i.op==Op::lh || i.op==Op::lhu ||
                 i.op==Op::lw || i.op==Op::lwu || i.op==Op::ld || i.op==Op::lq) && i.rt!=load.rt) continue;
            break;
        }
    }
    std::set<std::uint32_t> result;
    for (const auto& [pc,store]:program.instructions) {
        if (store.op!=Op::sw || !store.rt) continue;
        if (std::none_of(slots.begin(),slots.end(),[&](const auto& slot) { return slot.first==store.immediate; })) continue;
        unsigned fuel=128;
        const auto table=candidate(bytes,elf,program,pc,store.rt,fuel,0xffffffffU);
        if (!table) continue;
        for (const auto& [field,offset]:slots) {
            if (field!=store.immediate) continue;
            const auto pointer=initial_word(bytes,elf,*table+offset);
            if (!pointer) continue;
            if (const auto entry=executable_pointer(elf,*pointer)) result.insert(*entry);
        }
    }
    return result;
}
std::set<std::uint32_t> registered_code_pointers(ByteView bytes,const elf::Executable& elf,const Program& program) {
    std::set<std::uint32_t> result;
    std::map<std::uint32_t,unsigned> summaries;
    for (const auto& [pc,call]:program.instructions) {
        if (call.op!=r5900::Op::jal || pc>0xfffffff7U) continue;
        for (unsigned reg=4;reg<12;++reg) {
            unsigned fuel=128;
            const auto value=candidate(bytes,elf,program,pc+8,reg,fuel,pc);
            if (!value) continue;
            const auto address=executable_pointer(elf,*value);
            if (!address) continue;
            auto found=summaries.find(call.target());
            if (found==summaries.end()) found=summaries.emplace(call.target(),stored_arguments(program,call.target())).first;
            if (found->second&(1U<<(reg-4))) result.insert(*address);
        }
    }
    return result;
}
// Recover entry pointers from static descriptors and local stack descriptors.
// These are additional compilation candidates, not proofs of runtime values.
std::set<std::uint32_t> thread_entries(ByteView bytes,const elf::Executable& elf,const Program& program) {
    using r5900::Op;
    std::set<std::uint32_t> result;
    // SetupThread registers the common native return continuation in t0.
    // Treat it as another compilation root, not a runtime address assumption.
    for (const auto& [pc,instruction]:program.instructions) {
        if (instruction.op!=Op::syscall) continue;
        unsigned fuel=128;
        const auto number=candidate(bytes,elf,program,pc,3,fuel,0xffffffffU);
        if (!number || *number!=0x3c) continue;
        fuel=128;
        const auto root=candidate(bytes,elf,program,pc,8,fuel,0xffffffffU);
        if (root) if (const auto entry=executable_pointer(elf,*root)) result.insert(*entry);
    }
    for (const auto& [pc,call]:program.instructions) {
        if (call.op!=Op::jal || pc>0xfffffff7U) continue;
        const auto setup=program.instructions.find(call.target());
        const auto boundary=program.instructions.find(call.target()+4);
        if (setup==program.instructions.end() || boundary==program.instructions.end()) continue;
        const auto& instruction=setup->second;
        if (instruction.rs || instruction.rt!=3 || instruction.immediate!=0x20 ||
            (instruction.op!=Op::addiu && instruction.op!=Op::daddiu && instruction.op!=Op::ori) ||
            boundary->second.op!=Op::syscall) continue;
        unsigned fuel=128;
        const auto descriptor=candidate(bytes,elf,program,pc+8,4,fuel,pc);
        std::optional<std::uint32_t> entry;

        // The common local form uses a0 = sp + offset. Walk only the local
        // straight-line sequence; changes to sp or another call end the search.
        std::optional<std::int32_t> offset;
        auto before=pc+8;
        for (unsigned steps=0;steps<128 && before>=4 && !offset;++steps) {
            before-=4;
            const auto found=program.instructions.find(before);
            if (found==program.instructions.end()) break;
            const auto& i=found->second;
            if (before==pc) continue;
            if (i.branch() || i.terminal() || i.op==Op::syscall) break;
            if ((i.op==Op::addiu || i.op==Op::daddiu) && i.rt==4 && i.rs==29)
                offset=static_cast<std::int32_t>(i.immediate)+4;
            if ((i.op==Op::addu || i.op==Op::daddu || i.op==Op::bit_or) && i.rd==4 &&
                ((i.rs==29 && !i.rt) || (i.rt==29 && !i.rs))) offset=4;
        }
        before=pc+8;
        for (unsigned steps=0;steps<128 && before>=4;++steps) {
            before-=4;
            const auto found=program.instructions.find(before);
            if (found==program.instructions.end()) break;
            const auto& i=found->second;
            if (before==pc) continue;
            if (i.branch() || i.terminal() || i.op==Op::syscall) break;
            if (i.op==Op::sw) {
                bool matches=offset && i.rs==29 && i.immediate==*offset;
                if (descriptor) {
                    unsigned base_fuel=128;
                    const auto base=candidate(bytes,elf,program,before,i.rs,base_fuel,pc);
                    matches|=base && *base+static_cast<std::uint32_t>(i.immediate)==*descriptor+4;
                }
                if (matches) {
                    unsigned value_fuel=128;
                    entry=candidate(bytes,elf,program,before,i.rt,value_fuel,pc);
                    // An unresolved last store must not fall back to initial data.
                    if (!entry) entry=0;
                    break;
                }
            }
            switch (i.op) {
            case Op::sw: case Op::sd: case Op::sq: case Op::sb: case Op::sh: break;
            case Op::lui: case Op::ori: case Op::addiu: case Op::daddiu: case Op::lw: case Op::lwu:
                if (i.rt==29) before=0;
                break;
            case Op::addu: case Op::daddu: case Op::bit_or: case Op::sll:
                if (i.rd==29) before=0;
                break;
            default: before=0; break;
            }
        }
        if (!entry && descriptor && *descriptor<=0xfffffffbU) entry=initial_word(bytes,elf,*descriptor+4);
        if (!entry) continue;
        const auto address=*entry>=0x80000000U && *entry<0xc0000000U ? *entry&0x1fffffffU : *entry;
        if (address%4) continue;
        for (const auto& segment:elf.segments)
            if ((segment.flags&1) && address>=segment.address &&
                std::uint64_t(address)+4<=std::uint64_t(segment.address)+segment.file_size)
                result.insert(address);
    }
    return result;
}
void describe(std::ostream& out, const Program& p) {
    out << "Discovered instructions: " << p.instructions.size() << '\n';
    for (const auto& [pc, i] : p.instructions) {
        out << hex32(pc) << ' ' << hex32(i.word) << ' ' << r5900::name(i.op);
        if (i.branch()) out << (i.indirect() ? " indirect" : " -> " + hex32(i.target()));
        out << '\n';
    }
    for (auto pc : p.unresolved) out << "Unmapped target: " << hex32(pc) << '\n';
}
}
