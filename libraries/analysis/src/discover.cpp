#include <nt/analysis.hpp>
#include <array>
#include <algorithm>
#include <deque>
#include <optional>
#include <stdexcept>
#include <tuple>

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
            if (!elf::executable_address(executable,pc,16)) continue;
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
        if (pc % 4 || !elf::executable_address(elf,pc,4,true)) return {};
        for (const auto& s : elf.segments)
            if (((s.flags & 1) || elf.inferred_permissions) && pc >= s.address && std::uint64_t(pc) + 4 <= std::uint64_t(s.address) + s.file_size)
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
        if (result.instructions.size() > limit)
            throw std::runtime_error("Analysis instruction limit exceeded ("+std::to_string(limit)+") at "+hex32(pc));
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
std::optional<std::uint32_t> initial_word(ByteView bytes,const elf::Executable& elf,std::uint32_t address,
                                          unsigned width=4,bool sign=false) {
    if (address>=0x80000000U && address<0xc0000000U) address&=0x1fffffffU;
    if ((width!=1 && width!=2 && width!=4) || address%width) return {};
    for (const auto& segment:elf.segments) {
        if (address<segment.address || std::uint64_t(address)+width>std::uint64_t(segment.address)+segment.file_size) continue;
        const auto offset=std::size_t(segment.offset)+address-segment.address;
        if (offset>bytes.size() || width>bytes.size()-offset) return {};
        std::uint32_t value=0;
        for (unsigned n=0;n<width;++n) value|=std::uint32_t(bytes[offset+n])<<(8*n);
        const auto high=1U<<(width*8-1);
        return sign ? (value^high)-high : value;
    }
    return {};
}
std::optional<std::uint32_t> executable_pointer(const elf::Executable& elf,std::uint32_t value) {
    if (value>=0x80000000U && value<0xc0000000U) value&=0x1fffffffU;
    if (!value || value%4) return {};
    if (elf::executable_address(elf,value)) return value;
    return {};
}
// Track exact argument copies, not general data dependencies. A bounded callee
// summary identifies arguments written to word-sized callback/data slots.
struct ArgumentValue {
    unsigned origin=0;
    std::uint32_t offset=0;
    bool loaded=false;
    auto operator<=>(const ArgumentValue&) const=default;
};
struct ArgumentUses {
    unsigned stored=0,called=0;
    unsigned returned=0,data_read=0;
    struct Store {
        unsigned source,base;
        std::uint32_t offset,pc;
        unsigned base_register;
        auto operator<=>(const Store&) const=default;
    };
    std::set<Store> stores;
    std::map<unsigned,std::set<std::uint32_t>> callable_fields;
};
bool conditional_call(const r5900::Instruction& i) {
    using r5900::Op;
    return i.op==Op::bltzal || i.op==Op::bgezal || i.op==Op::bltzall || i.op==Op::bgezall;
}
unsigned link_register(const r5900::Instruction& i) {
    if (i.op==r5900::Op::jal || conditional_call(i)) return 31;
    return i.op==r5900::Op::jalr ? i.rd : 0;
}
bool call_transfer(const r5900::Instruction& i) {
    using r5900::Op;
    return i.op==Op::j || i.op==Op::jal || conditional_call(i) ||
        (i.indirect() && !(i.rs==31 && !link_register(i)));
}
bool scalar_float_preserves_gpr(r5900::Op op) {
    using r5900::Op;
    if (op>=Op::mov_s && op<=Op::compare_s) return true;
    switch (op) {
    case Op::mtc1: case Op::ctc1: case Op::lwc1: case Op::swc1:
    case Op::adda_s: case Op::suba_s: case Op::mula_s: case Op::sqrt_s:
    case Op::rsqrt_s: case Op::madd_s: case Op::msub_s: case Op::madda_s: case Op::msuba_s: return true;
    default: return false;
    }
}
// Limited effect model shared by the pointer slices. Unknown effects end a
// slice; they must not be assumed to preserve a potential pointer register.
std::optional<unsigned> pointer_write(const r5900::Instruction& i) {
    using r5900::Op;
    if (scalar_float_preserves_gpr(i.op)) return 0;
    switch (i.op) {
    case Op::sw: case Op::sd: case Op::sq: case Op::sb: case Op::sh: case Op::sync: return 0;
    case Op::slti: case Op::sltiu: case Op::andi: case Op::xori:
    case Op::lui: case Op::addiu: case Op::daddiu: case Op::ori: case Op::lw: case Op::lwu:
    case Op::lb: case Op::lbu: case Op::lh: case Op::lhu: case Op::ld: case Op::lq: case Op::mfc1: case Op::cfc1:
        return i.rt;
    case Op::slt: case Op::sltu: case Op::sll: case Op::srl: case Op::sra:
    case Op::sllv: case Op::srlv: case Op::srav: case Op::addu: case Op::daddu: case Op::bit_or:
    case Op::bit_and: case Op::bit_xor: case Op::bit_nor: case Op::subu: case Op::dsubu:
    case Op::mult: case Op::multu: case Op::mflo: case Op::mfhi: case Op::movn: case Op::movz:
        return i.rd;
    default: return {};
    }
}
std::optional<unsigned> copied_register(const r5900::Instruction& i) {
    using r5900::Op;
    if (i.op==Op::addu || i.op==Op::daddu || i.op==Op::bit_or) {
        if (!i.rt) return i.rs;
        if (!i.rs) return i.rt;
    }
    if (i.op==Op::sll && !i.sa) return i.rt;
    if ((i.op==Op::addiu || i.op==Op::daddiu || i.op==Op::ori) && !i.immediate) return i.rs;
    return {};
}
unsigned pointer_store_width(r5900::Op op) {
    using r5900::Op;
    switch (op) {
    case Op::sb: return 1;
    case Op::sh: return 2;
    case Op::sw: case Op::swc1: return 4;
    case Op::sd: return 8;
    case Op::sq: return 16;
    default: return 0;
    }
}
bool overwrites_word(std::uint32_t store,unsigned width,std::uint32_t word) {
    // Unsigned differences also handle a store spanning the address wrap.
    return word-store<width || store-word<4;
}
ArgumentUses argument_uses(const Program& program,std::uint32_t entry) {
    using r5900::Op;
    using Origins=std::array<ArgumentValue,32>;
    using Stack=std::map<std::int32_t,ArgumentValue>;
    constexpr unsigned stack_origin=1U<<31;
    Origins initial{};
    // The EE ABI passes eight integer arguments in registers a0 through t3.
    for (unsigned reg=4;reg<12;++reg) initial[reg].origin=1U<<(reg-4);
    initial[29].origin=stack_origin;
    Stack incoming;
    for (unsigned n=0;n<16;++n) incoming[static_cast<std::int32_t>(n*4)]={1U<<(8+n)};
    struct Cursor { std::uint32_t pc; Origins origins; Stack stack; unsigned call_depth; };
    std::deque<Cursor> pending{{entry,initial,incoming,0}};
    std::set<std::tuple<std::uint32_t,Origins,Stack>> visited;
    ArgumentUses uses;
    unsigned budget=1024;
    const auto transfer=[&](const r5900::Instruction& i,Origins& origins,Stack& stack) {
        if (scalar_float_preserves_gpr(i.op) && i.op!=Op::swc1) return true;
        unsigned destination=0;
        ArgumentValue value;
        const auto& base=origins[i.rs];
        const bool stack_access=base.origin==stack_origin && !base.loaded;
        const auto offset=base.offset+static_cast<std::uint32_t>(i.immediate);
        switch (i.op) {
        case Op::sw: case Op::sb: case Op::sh: case Op::sd: case Op::sq: case Op::swc1: {
            const auto source=i.op==Op::swc1 ? ArgumentValue{} : origins[i.rt];
            if (i.op==Op::sw && !source.loaded && !source.offset && source.origin!=stack_origin) {
                uses.stored|=source.origin;
                if (!base.loaded && source.origin)
                    uses.stores.insert({source.origin,base.origin,offset,i.pc,i.rs});
            }
            if (stack_access) {
                const auto at=static_cast<std::int32_t>(offset);
                const auto width=pointer_store_width(i.op);
                std::erase_if(stack,[&](const auto& slot) {
                    return std::int64_t(slot.first)<std::int64_t(at)+width && std::int64_t(at)<std::int64_t(slot.first)+4;
                });
                if (width>=4 && source.origin) stack[at]=source;
            }
            return true;
        }
        case Op::addu: case Op::daddu: case Op::bit_or:
            destination=i.rd;
            if (!i.rt) value=origins[i.rs];
            else if (!i.rs) value=origins[i.rt];
            break;
        case Op::addiu: case Op::daddiu:
            destination=i.rt;
            if (origins[i.rs].origin && !origins[i.rs].loaded) {
                value=origins[i.rs]; value.offset+=static_cast<std::uint32_t>(i.immediate);
            }
            break;
        case Op::ori: destination=i.rt; if (!i.immediate) value=origins[i.rs]; break;
        case Op::sll: destination=i.rd; if (!i.sa) value=origins[i.rt]; break;
        case Op::lw: case Op::lwu: case Op::ld: case Op::lq:
            destination=i.rt;
            if (stack_access) {
                const auto at=static_cast<std::int32_t>(offset);
                if (const auto found=stack.find(at);found!=stack.end()) value=found->second;
            } else if (base.origin && !base.loaded) {
                uses.data_read|=base.origin;
                if (i.op==Op::lw || i.op==Op::lwu) { value=base; value.offset=offset; value.loaded=true; }
            }
            break;
        case Op::lb: case Op::lbu: case Op::lh: case Op::lhu:
            if (base.origin && !base.loaded && !stack_access) uses.data_read|=base.origin;
            destination=i.rt; break;
        case Op::lui: case Op::mfc1: case Op::cfc1:
        case Op::andi: case Op::xori: case Op::slti: case Op::sltiu:
            destination=i.rt; break;
        case Op::bit_and: case Op::bit_xor: case Op::bit_nor: case Op::subu:
        case Op::dsubu: case Op::slt: case Op::sltu: case Op::srl: case Op::sra:
        case Op::sllv: case Op::srlv: case Op::srav: case Op::mult: case Op::multu:
        case Op::mflo: case Op::mfhi: case Op::movn: case Op::movz:
            destination=i.rd; break;
        default: return false;
        }
        if (destination) origins[destination]=value;
        return true;
    };
    while (!pending.empty() && budget--) {
        auto [pc,origins,stack,call_depth]=std::move(pending.front()); pending.pop_front();
        if (!visited.emplace(pc,origins,stack).second || pc>0xfffffff7U) continue;
        const auto found=program.instructions.find(pc);
        if (found==program.instructions.end()) continue;
        const auto& i=found->second;
        if (!i.branch()) {
            if (transfer(i,origins,stack)) pending.push_back({pc+4,origins,stack,call_depth});
            continue;
        }
        if (i.indirect() && origins[i.rs].loaded)
            uses.callable_fields[origins[i.rs].origin].insert(origins[i.rs].offset);
        else if (i.indirect() && !origins[i.rs].offset)
            uses.called|=origins[i.rs].origin;
        // ABI-preserved copies may reach registration after a helper call.
        // This only proposes compilation roots; it never folds runtime values.
        if (const auto link=link_register(i)) {
            origins[link]={};
            const auto before_slot=origins;
            const auto stack_before_slot=stack;
            const auto slot=program.instructions.find(pc+4);
            if (slot==program.instructions.end() || !transfer(slot->second,origins,stack)) continue;
            // Registration wrappers often forward a callback to another helper.
            // Follow bounded direct calls with the current argument origins;
            // their effects are discovery evidence, never folded runtime values.
            if (!i.indirect() && call_depth<4)
                pending.push_back({i.target(),origins,stack,call_depth+1});
            if (conditional_call(i))
                pending.push_back({pc+8,i.likely() ? before_slot : origins,i.likely()?stack_before_slot:stack,call_depth});
            for (unsigned reg=1;reg<32;++reg)
                if (!(reg>=16 && reg<=23) && reg!=28 && reg!=29 && reg!=30) origins[reg]={};
            // Retain stack values as possible compilation candidates across a
            // call, not immutable memory. Runtime loads still read the slot.
            pending.push_back({pc+8,origins,stack,call_depth});
            continue;
        }
        const auto before_slot=origins;
        const auto stack_before_slot=stack;
        const auto slot=program.instructions.find(pc+4);
        if (slot==program.instructions.end() || !transfer(slot->second,origins,stack)) continue;
        if (i.indirect() && i.rs==31 && !origins[2].loaded && !origins[2].offset) uses.returned|=origins[2].origin;
        if (!i.indirect()) pending.push_back({i.target(),origins,stack,call_depth});
        if (i.op!=Op::j && i.op!=Op::jr)
            pending.push_back({pc+8,i.likely() ? before_slot : origins,i.likely()?stack_before_slot:stack,call_depth});
    }
    return uses;
}
// Bounded backward slicing is used only to propose code for compilation. A load
// reads the initial ELF image, not a claim that writable data stays constant.
std::optional<std::uint32_t> leaf_return_value(ByteView bytes,const elf::Executable& elf,
    const Program& program,std::uint32_t entry,unsigned& fuel);
std::optional<std::uint32_t> candidate(ByteView bytes,const elf::Executable& elf,const Program& program,
                                     std::uint32_t before,unsigned reg,unsigned& fuel,std::uint32_t call_site,bool layout_candidate=false,
                                     std::uint32_t conditional_store=0,bool follow_returns=false) {
    using r5900::Op;
    if (!reg) return 0;
    while (before>=4 && fuel) {
        --fuel; before-=4;
        const auto found=program.instructions.find(before);
        if (found==program.instructions.end()) return {};
        const auto& i=found->second;
        if (scalar_float_preserves_gpr(i.op)) continue;
        if (before==call_site && (call_transfer(i) || i.indirect())) {
            if (reg==link_register(i)) return {};
            continue;
        }
        if (link_register(i)) {
            // Follow a returned constant only when a consumer is already being
            // sliced. Scanning arbitrary return values would misclassify data
            // addresses in mixed executable/data segments as functions.
            if (follow_returns && reg==2 && i.op==Op::jal)
                return leaf_return_value(bytes,elf,program,i.target(),fuel);
            // Saved registers are ABI candidates across calls, never proven values.
            if (reg>=16 && reg<=23) continue;
            return {};
        }
        if (i.branch() || i.terminal() || i.op==Op::syscall) {
            // A non-linking branch may skip the store entirely. On its
            // fallthrough path it does not change this register. This is only
            // a stored-code discovery candidate, never a folded runtime value
            // or a native memory-layout assertion.
            if (conditional_store && i.target()>conditional_store) {
                switch (i.op) {
                case Op::beq: case Op::bne: case Op::blez: case Op::bgtz:
                case Op::bltz: case Op::bgez: case Op::bc1f: case Op::bc1t: continue;
                default: break;
                }
            }
            if (layout_candidate && reg>=16 && reg<=23) continue;
            return {};
        }
        if (layout_candidate && (i.op==Op::mtc0 || i.op==Op::sync)) continue;
        unsigned destination=0;
        switch (i.op) {
        case Op::sw: case Op::sd: case Op::sq: case Op::sb: case Op::sh: continue;
        case Op::slti: case Op::sltiu:
        case Op::lui: case Op::addiu: case Op::daddiu: case Op::ori: case Op::lw: case Op::lwu:
        case Op::lb: case Op::lbu: case Op::lh: case Op::lhu: case Op::ld: case Op::lq: case Op::mfc1: case Op::cfc1:
            destination=i.rt; break;
        case Op::slt: case Op::sltu:
        case Op::addu: case Op::daddu: case Op::bit_or: case Op::sll:
            destination=i.rd; break;
        default: return {};
        }
        if (destination!=reg) continue;
        if (i.op==Op::ld || i.op==Op::lq || i.op==Op::mfc1 || i.op==Op::cfc1) return {};
        if (i.op==Op::slti || i.op==Op::sltiu || i.op==Op::slt || i.op==Op::sltu) return {};
        if (i.op==Op::lui) return static_cast<std::uint32_t>(i.immediate)<<16;
        const auto a=candidate(bytes,elf,program,before,i.rs,fuel,call_site,layout_candidate,conditional_store,follow_returns);
        if (!a) return {};
        if (i.op==Op::addiu || i.op==Op::daddiu) return *a+static_cast<std::uint32_t>(i.immediate);
        if (i.op==Op::ori) return *a|static_cast<std::uint16_t>(i.immediate);
        if (i.op==Op::lw || i.op==Op::lwu || i.op==Op::lb || i.op==Op::lbu || i.op==Op::lh || i.op==Op::lhu) {
            const auto width=i.op==Op::lb || i.op==Op::lbu ? 1U : i.op==Op::lh || i.op==Op::lhu ? 2U : 4U;
            return initial_word(bytes,elf,*a+static_cast<std::uint32_t>(i.immediate),width,i.op==Op::lb || i.op==Op::lh);
        }
        const auto b=candidate(bytes,elf,program,before,i.rt,fuel,call_site,layout_candidate,conditional_store,follow_returns);
        if (!b) return {};
        if (i.op==Op::sll) return *b<<i.sa;
        return i.op==Op::bit_or ? *a|*b : *a+*b;
    }
    return {};
}
std::optional<std::uint32_t> leaf_return_value(ByteView bytes,const elf::Executable& elf,
    const Program& program,std::uint32_t entry,unsigned& fuel) {
    std::deque<std::uint32_t> pending{entry};
    std::set<std::uint32_t> visited;
    std::optional<std::uint32_t> result;
    while (!pending.empty()) {
        if (!fuel) return {};
        --fuel;
        const auto pc=pending.front(); pending.pop_front();
        if (!visited.insert(pc).second) continue;
        const auto found=program.instructions.find(pc);
        if (found==program.instructions.end() || pc>0xfffffff7U) return {};
        const auto& i=found->second;
        if (i.indirect() && i.rs==31 && !link_register(i)) {
            const auto value=candidate(bytes,elf,program,pc+8,2,fuel,pc);
            if (!value || (result && *result!=*value)) return {};
            result=value;
        } else if (i.terminal() || i.op==r5900::Op::syscall || link_register(i) || i.indirect()) return {};
        else if (i.branch()) {
            pending.push_back(i.target());
            if (i.op!=r5900::Op::j) pending.push_back(pc+8);
        } else pending.push_back(pc+4);
    }
    return result;
}
std::optional<std::uint32_t> known_callee(ByteView bytes,const elf::Executable& elf,
                                         const Program& program,const r5900::Instruction& call) {
    if (!call_transfer(call)) return {};
    if (!call.indirect()) return call.target();
    unsigned fuel=128;
    // The target is consumed before the delay slot or link-register write.
    const auto value=candidate(bytes,elf,program,call.pc,call.rs,fuel,0xffffffffU,false,0,true);
    return value ? executable_pointer(elf,*value) : std::nullopt;
}
struct PointerLoad { std::uint32_t pc; unsigned base; std::uint32_t offset; };
std::optional<PointerLoad> called_load(const Program& program,const r5900::Instruction& call) {
    using r5900::Op;
    unsigned reg=call.rs;
    auto before=call.pc;
    for (unsigned step=0;step<128 && before>=4;++step) {
        before-=4;
        const auto found=program.instructions.find(before);
        if (found==program.instructions.end()) return {};
        const auto& i=found->second;
        if (i.branch() || i.terminal() || i.op==Op::syscall) return {};
        const auto write=pointer_write(i);
        if (!write) return {};
        if (*write!=reg) continue;
        if (i.op==Op::lw || i.op==Op::lwu) return PointerLoad{i.pc,i.rs,static_cast<std::uint32_t>(i.immediate)};
        const auto copy=copied_register(i);
        if (!copy || !*copy) return {};
        reg=*copy;
    }
    return {};
}
bool same_local_slot(const Program& program,const PointerLoad& load,std::uint32_t after,unsigned base,std::uint32_t offset) {
    using r5900::Op;
    auto before=load.pc;
    auto reg=load.base;
    auto displacement=load.offset;
    for (unsigned step=0;before>after && step<128;++step) {
        before-=4;
        const auto found=program.instructions.find(before);
        if (found==program.instructions.end()) return false;
        const auto& i=found->second;
        if (i.branch() || i.terminal() || i.op==Op::syscall) return false;
        // A definite overwrite severs the earlier store-to-call relation.
        if (const auto width=pointer_store_width(i.op);width && i.rs==reg &&
            overwrites_word(static_cast<std::uint32_t>(i.immediate),width,displacement)) return false;
        const auto write=pointer_write(i);
        if (!write) return false;
        if (*write!=reg) continue;
        if (const auto copy=copied_register(i)) reg=*copy;
        else if (i.op==Op::addiu || i.op==Op::daddiu) {
            reg=i.rs; displacement+=static_cast<std::uint32_t>(i.immediate);
        } else return false;
    }
    return before==after && reg==base && displacement==offset;
}
struct CallConsumers {
    std::set<std::pair<bool,std::uint32_t>> slots; // absolute address, or ABI GP-relative offset
    std::vector<PointerLoad> loads;
    CallConsumers(ByteView bytes,const elf::Executable& elf,const Program& program) {
        for (const auto& [pc,i]:program.instructions) {
            if (!i.indirect() || !call_transfer(i)) continue;
            const auto load=called_load(program,i);
            if (!load) continue;
            loads.push_back(*load);
            unsigned fuel=128;
            const auto base=candidate(bytes,elf,program,load->pc,load->base,fuel,0xffffffffU);
            if (base) slots.insert({false,physical(*base+load->offset)});
            else if (load->base==28) slots.insert({true,load->offset});
        }
        std::sort(loads.begin(),loads.end(),[](const auto& a,const auto& b) { return a.pc<b.pc; });
    }
    static std::uint32_t physical(std::uint32_t address) {
        return address>=0x80000000U && address<0xc0000000U ? address&0x1fffffffU : address;
    }
    bool contains(ByteView bytes,const elf::Executable& elf,const Program& program,
                  std::uint32_t after,unsigned base,std::uint32_t offset,std::uint32_t call_site=0xffffffffU) const {
        unsigned fuel=128;
        const auto address=candidate(bytes,elf,program,after,base,fuel,call_site);
        if (address && slots.contains({false,physical(*address+offset)})) return true;
        if (!address && base==28 && slots.contains({true,offset})) return true;
        auto load=std::lower_bound(loads.begin(),loads.end(),after,[](const auto& item,auto pc) { return item.pc<pc; });
        for (;load!=loads.end() && std::uint64_t(load->pc)<=std::uint64_t(after)+512;++load)
            if (same_local_slot(program,*load,after,base,offset)) return true;
        return false;
    }
};
std::optional<std::uint32_t> stack_argument(ByteView bytes,const elf::Executable& elf,const Program& program,
    std::uint32_t call,unsigned index) {
    using r5900::Op;
    auto before=call+8;
    std::uint32_t offset=index*4;
    for (unsigned step=0;before>=4 && step<128;++step) {
        before-=4;
        const auto found=program.instructions.find(before);
        if (found==program.instructions.end()) return {};
        const auto& i=found->second;
        if (before==call) continue;
        if (i.branch() || i.terminal() || i.op==Op::syscall) return {};
        if (const auto width=pointer_store_width(i.op);width && i.rs==29 &&
            overwrites_word(static_cast<std::uint32_t>(i.immediate),width,offset)) {
            if (i.op!=Op::sw || static_cast<std::uint32_t>(i.immediate)!=offset) return {};
            unsigned fuel=128;
            return candidate(bytes,elf,program,before,i.rt,fuel,call);
        }
        const auto write=pointer_write(i);
        if (!write) return {};
        if (*write==29) {
            if ((i.op!=Op::addiu && i.op!=Op::daddiu) || i.rs!=29) return {};
            offset+=static_cast<std::uint32_t>(i.immediate);
        }
    }
    return {};
}
}
std::string_view name(CallKind kind) {
    switch (kind) {
    case CallKind::direct: return "direct-call";
    case CallKind::conditional: return "conditional-call";
    case CallKind::indirect: return "indirect-call";
    case CallKind::direct_jump: return "direct-jump-or-tail-call";
    case CallKind::indirect_jump: return "indirect-jump-or-tail-call";
    case CallKind::return_link: return "return";
    case CallKind::return_exception: return "exception-return";
    case CallKind::service: return "syscall";
    }
    return "unknown";
}
std::vector<CallSite> call_sites(ByteView bytes,const elf::Executable& elf,const Program& program) {
    std::vector<CallSite> result;
    using r5900::Op;
    for (const auto& [pc,i]:program.instructions) {
        std::optional<CallKind> kind;
        if (i.op==Op::syscall) kind=CallKind::service;
        else if (i.op==Op::eret) kind=CallKind::return_exception;
        else if (i.indirect() && i.rs==31 && !link_register(i)) kind=CallKind::return_link;
        else if (conditional_call(i)) kind=CallKind::conditional;
        else if (i.op==Op::jal) kind=CallKind::direct;
        else if (i.op==Op::j) kind=CallKind::direct_jump;
        else if (i.indirect()) kind=link_register(i) ? CallKind::indirect : CallKind::indirect_jump;
        if (kind) result.push_back({pc,*kind,known_callee(bytes,elf,program,i),i.indirect() && call_transfer(i)});
    }
    return result;
}
std::vector<LayoutCandidate> layout_candidates(ByteView bytes,const elf::Executable& elf,const Program& program) {
    using r5900::Op;
    std::vector<LayoutCandidate> result;
    std::set<std::uint32_t> sources;
    for (const auto& [pc,call]:program.instructions) {
        const auto target=known_callee(bytes,elf,program,call);
        if (!target || pc<12 || *target>0xfffffffBU) continue;
        const auto setup=program.instructions.find(*target),trap=program.instructions.find(*target+4);
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
        const auto callee=known_callee(bytes,elf,program,call);
        if (!callee || pc>0xfffffff7U || *callee>0xfffffffBU) continue;
        const auto setup=program.instructions.find(*callee);
        const auto boundary=program.instructions.find(*callee+4);
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
        if (elf::executable_address(elf,address)) {
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
std::set<std::uint32_t> stored_code_pointers(ByteView bytes,const elf::Executable& elf,const Program& program,
                                           std::vector<PointerReference>* references) {
    std::set<std::uint32_t> result;
    const CallConsumers consumers(bytes,elf,program);
    for (const auto& [pc,i]:program.instructions) {
        if (i.op!=r5900::Op::sw || !i.rt) continue;
        unsigned fuel=128;
        // A call delay-slot store consumes the value before the callee runs.
        const auto prior=pc>=4 ? program.instructions.find(pc-4) : program.instructions.end();
        const auto call_site=prior!=program.instructions.end() && call_transfer(prior->second) ? pc-4 : 0xffffffffU;
        const auto value=candidate(bytes,elf,program,pc,i.rt,fuel,call_site,false,pc);
        if (!value || !*value) continue;
        if (const auto address=executable_pointer(elf,*value)) {
            const bool callable=consumers.contains(bytes,elf,program,pc+4,i.rs,static_cast<std::uint32_t>(i.immediate),call_site);
            if (callable) result.insert(*address);
            if (references) references->push_back({pc,*address,false,callable,false});
        }
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
std::set<std::uint32_t> initial_indirect_entries(ByteView bytes,const elf::Executable& elf,const Program& program) {
    std::set<std::uint32_t> result;
    for (const auto& [pc,call]:program.instructions) {
        if (!call.indirect() || !call.rs || (call.op==r5900::Op::jr && call.rs==31)) continue;
        unsigned fuel=128;
        const auto value=candidate(bytes,elf,program,pc,call.rs,fuel,0xffffffffU,false,0,true);
        if (value) if (const auto entry=executable_pointer(elf,*value)) result.insert(*entry);
    }
    return result;
}
std::set<std::uint32_t> referenced_lifecycle_entries(ByteView bytes,const elf::Executable& elf,const Program& program) {
    std::set<std::uint32_t> result,visited;
    constexpr unsigned maximum=4096;
    for (const auto& [pc,load]:program.instructions) {
        if (load.op!=r5900::Op::lw || load.immediate || !load.rs) continue;
        unsigned fuel=128;
        const auto base=candidate(bytes,elf,program,pc,load.rs,fuel,0xffffffffU);
        if (!base || *base%4 || !visited.insert(*base).second) continue;
        const auto header=initial_word(bytes,elf,*base);
        if (!header || (*header!=0xffffffffU && (!*header || *header>maximum))) continue;
        std::set<std::uint32_t> entries;
        bool terminated=false;
        for (unsigned index=1;index<=maximum+1;++index) {
            const auto address=std::uint64_t(*base)+4*index;
            if (address>0xfffffffcU) break;
            const auto word=initial_word(bytes,elf,static_cast<std::uint32_t>(address));
            if (!word) break;
            if (!*word) { terminated=*header==0xffffffffU || index-1==*header; break; }
            if (index>maximum || (*header!=0xffffffffU && index>*header)) break;
            const auto entry=executable_pointer(elf,*word);
            if (!entry) break;
            entries.insert(*entry);
        }
        if (terminated) result.insert(entries.begin(),entries.end());
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
                if (call!=program.instructions.end() && call->second.indirect() &&
                    call->second.rs==i.rt && call_transfer(call->second)) {
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
std::set<std::uint32_t> registered_code_pointers(ByteView bytes,const elf::Executable& elf,const Program& program,
                                               std::vector<PointerReference>* references) {
    std::set<std::uint32_t> result;
    std::map<std::uint32_t,ArgumentUses> summaries;
    const CallConsumers consumers(bytes,elf,program);
    for (const auto& [pc,call]:program.instructions) {
        // A tail jump passes the same argument registers as a linking call.
        // Recover both before lowering: an indirect callback may be saved by
        // the destination even though this wrapper never creates a new link.
        const auto callee=known_callee(bytes,elf,program,call);
        if (!callee || pc>0xfffffff7U) continue;
        auto found=summaries.find(*callee);
        if (found==summaries.end()) found=summaries.emplace(*callee,argument_uses(program,*callee)).first;
        const auto& uses=found->second;
        const auto argument=[&](unsigned index,bool called=false) {
            unsigned fuel=128;
            return index<8 ? candidate(bytes,elf,program,pc+8,index+4,fuel,pc,false,0,called) :
                stack_argument(bytes,elf,program,pc,index-8);
        };
        for (unsigned index=0;index<24;++index) {
            const auto bit=1U<<index;
            if (!((uses.stored|uses.called|uses.data_read)&bit)) continue;
            bool callable=(uses.called&bit)!=0;
            const auto value=argument(index,callable);
            if (!value) continue;
            const auto address=executable_pointer(elf,*value);
            if (!address) continue;
            for (const auto& store:uses.stores) {
                if (!(store.source&bit)) continue;
                const auto instruction=program.instructions.find(store.pc);
                if (instruction!=program.instructions.end() && consumers.contains(bytes,elf,program,store.pc+4,
                    store.base_register,static_cast<std::uint32_t>(instruction->second.immediate))) callable=true;
                if (const auto fields=uses.callable_fields.find(store.base);
                    fields!=uses.callable_fields.end() && fields->second.contains(store.offset)) callable=true;
                for (unsigned base_index=0;base_index<24 && !callable;++base_index) {
                    if (store.base!=(1U<<base_index)) continue;
                    const auto base=argument(base_index);
                    if (base && consumers.slots.contains({false,CallConsumers::physical(*base+store.offset)})) callable=true;
                    if (base_index<8 && consumers.contains(bytes,elf,program,pc+8,base_index+4,store.offset,pc)) callable=true;
                    if ((uses.returned&store.base) && consumers.contains(bytes,elf,program,pc+8,2,store.offset,pc)) callable=true;
                }
            }
            if (callable) result.insert(*address);
            if (references) references->push_back({pc,*address,true,callable,(uses.data_read&bit)!=0,*callee,index});
        }
    }
    // Multi-callback descriptor shapes come from actual indirect calls through
    // one argument. Matching initialized records are compilation candidates,
    // not assertions about mutable runtime data or an injected dispatch table.
    std::set<std::set<std::uint32_t>> shapes;
    for (const auto& [entry,uses]:summaries) for (const auto& [origin,fields]:uses.callable_fields)
        if (fields.size()>=2 && fields.size()<=16 && *fields.rbegin()<256 &&
            std::all_of(fields.begin(),fields.end(),[](auto offset) { return offset%4==0; })) shapes.insert(fields);
    for (const auto& segment:elf.segments) {
        if (segment.flags&1) continue;
        for (const auto& fields:shapes) {
            const auto extent=std::uint64_t(*fields.rbegin())+4;
            for (std::uint64_t offset=(4U-(segment.address&3U))&3U;offset+extent<=segment.file_size;offset+=4) {
                std::set<std::uint32_t> entries;
                bool valid=true;
                for (const auto field:fields) {
                    const auto address=std::uint64_t(segment.address)+offset+field;
                    const auto value=address<=0xfffffffcU ? initial_word(bytes,elf,static_cast<std::uint32_t>(address)) : std::nullopt;
                    const auto target=value ? executable_pointer(elf,*value) : std::nullopt;
                    if (!target) { valid=false; break; }
                    entries.insert(*target);
                }
                if (valid && entries.size()>=2) result.insert(entries.begin(),entries.end());
            }
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
        const auto callee=known_callee(bytes,elf,program,call);
        if (!callee || pc>0xfffffff7U || *callee>0xfffffffBU) continue;
        const auto setup=program.instructions.find(*callee);
        const auto boundary=program.instructions.find(*callee+4);
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
        if (elf::executable_address(elf,address)) result.insert(address);
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
