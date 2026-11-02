#include <nt/analysis.hpp>
#include "pointer_flow.hpp"
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
    result.entries.insert(elf.entry);
    result.entries.insert(elf.lifecycle_entries.begin(),elf.lifecycle_entries.end());
    result.entries.insert(additional_entries.begin(),additional_entries.end());
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
        if (!delay) result.ordinary.insert(pc);
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
    unsigned width=0;
    bool entry_value=true;
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
    std::set<Store> global_stores;
    struct ReferencedStore {
        unsigned source,base;
        std::uint32_t source_offset,offset;
        unsigned width;
    };
    std::vector<ReferencedStore> referenced_stores;
    std::map<unsigned,std::set<std::uint32_t>> callable_fields;
    std::map<unsigned,std::set<std::uint32_t>> registered_fields;
};
struct BoundArguments { unsigned function,context; };
using BoundCalls=std::map<std::uint32_t,std::vector<BoundArguments>>;
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
std::optional<unsigned> pointer_write(const r5900::Instruction& i) { return pointer_flow::write(i); }
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
    case Op::sw: case Op::swc1: case Op::swl: case Op::swr: return 4;
    case Op::sd: case Op::sdl: case Op::sdr: return 8;
    case Op::sq: case Op::sqc2: return 16;
    default: return 0;
    }
}
bool partial_store(r5900::Op op) {
    using r5900::Op;
    return op==Op::swl || op==Op::swr || op==Op::sdl || op==Op::sdr;
}
std::uint32_t store_range_begin(const r5900::Instruction& i) {
    const auto offset=std::uint32_t(i.immediate);
    // With unknown base alignment, a left-partial store can touch up to
    // width-1 bytes before its nominal address. Bound its possible footprint.
    return i.op==r5900::Op::swl || i.op==r5900::Op::sdl ? offset+1-pointer_store_width(i.op) : offset;
}
bool unaligned_pair(const r5900::Instruction& first,const r5900::Instruction& second,
                    r5900::Op left,r5900::Op right,unsigned extent) {
    return first.rs==second.rs && first.rt==second.rt &&
        ((first.op==left && second.op==right && first.immediate==second.immediate+int(extent)-1) ||
         (first.op==right && second.op==left && second.immediate==first.immediate+int(extent)-1));
}
bool overwrites_word(std::uint32_t store,unsigned width,std::uint32_t word) {
    // Unsigned differences also handle a store spanning the address wrap.
    return word-store<width || store-word<4;
}
ArgumentUses argument_uses(const Program& program,std::uint32_t entry,const BoundCalls* bindings=nullptr) {
    using r5900::Op;
    using Origins=std::array<ArgumentValue,32>;
    using Stack=std::map<std::int32_t,ArgumentValue>;
    using Writes=std::set<std::tuple<unsigned,std::uint32_t,unsigned>>;
    constexpr unsigned stack_origin=1U<<31;
    Origins initial{};
    // The EE ABI passes eight integer arguments in registers a0 through t3.
    for (unsigned reg=4;reg<12;++reg) initial[reg].origin=1U<<(reg-4);
    initial[29].origin=stack_origin;
    Stack incoming;
    for (unsigned n=0;n<16;++n) incoming[static_cast<std::int32_t>(n*4)]={1U<<(8+n)};
    struct Cursor { std::uint32_t pc; Origins origins; Stack stack; Writes writes; unsigned call_depth; };
    std::deque<Cursor> pending{{entry,initial,incoming,{},0}};
    std::set<std::tuple<std::uint32_t,Origins,Stack,Writes>> visited;
    ArgumentUses uses;
    unsigned budget=1024;
    const auto binding=[&](std::uint32_t pc,const Origins& origins) {
        if (!bindings) return;
        const auto found=bindings->find(pc);
        if (found==bindings->end()) return;
        for (const auto& arguments:found->second) {
            const auto& function=origins[arguments.function];
            const auto& context=origins[arguments.context];
            // The loaded member and its owning object are registered together.
            // An unrelated context does not establish a callable object field.
            if (function.loaded && function.width==4 && function.entry_value && function.origin &&
                context.origin==function.origin && !context.loaded && !context.offset)
                uses.registered_fields[function.origin].insert(function.offset);
            if (!function.loaded && !function.offset && function.origin && context.origin &&
                !context.loaded && !context.offset && context.origin!=function.origin)
                uses.called|=function.origin;
        }
    };
    const auto transfer=[&](const r5900::Instruction& i,Origins& origins,Stack& stack,Writes& writes) {
        if (scalar_float_preserves_gpr(i.op) && i.op!=Op::swc1) return true;
        unsigned destination=0;
        bool loaded_field=false;
        ArgumentValue value;
        const auto& base=origins[i.rs];
        const bool stack_access=base.origin==stack_origin && !base.loaded;
        const auto offset=base.offset+static_cast<std::uint32_t>(i.immediate);
        if (bindings && base.origin && !base.loaded && !stack_access)
            if (const auto width=pointer_store_width(i.op)) writes.emplace(base.origin,base.offset+store_range_begin(i),width);
        const auto pair=[&](Op left,Op right,unsigned extent) {
            const auto previous=program.instructions.find(i.pc-4);
            return previous!=program.instructions.end() && unaligned_pair(previous->second,i,left,right,extent);
        };
        switch (i.op) {
        case Op::ldl: case Op::ldr:
            destination=i.rt;
            if (i.rs!=i.rt && pair(Op::ldl,Op::ldr,8) && base.origin && !base.loaded) {
                value=base; value.offset=offset-(i.op==Op::ldl ? 7U : 0U);
                value.loaded=true; value.width=8; uses.data_read|=base.origin;
                loaded_field=true;
            }
            break;
        case Op::sdl: case Op::sdr: {
            const auto& source=origins[i.rt];
            if (pair(Op::sdl,Op::sdr,8) && source.loaded && source.width>=8 && !base.loaded)
                uses.referenced_stores.push_back({source.origin,base.origin,source.offset,
                    offset-(i.op==Op::sdl ? 7U : 0U),8});
            if (stack_access) stack.clear(); // Unknown alignment: invalidate incoming stack slots.
            return true;
        }
        case Op::sw: case Op::sb: case Op::sh: case Op::sd: case Op::sq: case Op::swc1: {
            const auto source=i.op==Op::swc1 ? ArgumentValue{} : origins[i.rt];
            const auto extent=pointer_store_width(i.op);
            if (i.op!=Op::swc1 && extent>=4 && source.loaded && source.origin &&
                source.width>=extent && (base.origin || extent>=8) && !base.loaded)
                uses.referenced_stores.push_back({source.origin,base.origin,source.offset,offset,extent});
            if (i.op==Op::sw && !source.loaded && !source.offset && source.origin!=stack_origin) {
                uses.stored|=source.origin;
                if (!base.loaded && source.origin)
                    uses.stores.insert({source.origin,base.origin,offset,i.pc,i.rs});
            }
            if (i.op==Op::sw && i.rt==28 && !base.loaded)
                uses.global_stores.insert({0,base.origin,offset,i.pc,i.rs});
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
                value=base; value.offset=offset; value.loaded=true;
                value.width=i.op==Op::lq ? 16U : i.op==Op::ld ? 8U : 4U;
                loaded_field=true;
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
        if (loaded_field) for (const auto& [origin,at,width]:writes)
            if (origin==value.origin && (value.offset-at<width || at-value.offset<value.width))
                value.entry_value=false;
        if (destination) origins[destination]=value;
        return true;
    };
    while (!pending.empty() && budget--) {
        auto [pc,origins,stack,writes,call_depth]=std::move(pending.front()); pending.pop_front();
        if (!visited.emplace(pc,origins,stack,writes).second || pc>0xfffffff7U) continue;
        const auto found=program.instructions.find(pc);
        if (found==program.instructions.end()) continue;
        const auto& i=found->second;
        if (!i.branch()) {
            if (transfer(i,origins,stack,writes)) pending.push_back({pc+4,origins,stack,writes,call_depth});
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
            const auto writes_before_slot=writes;
            const auto slot=program.instructions.find(pc+4);
            if (slot==program.instructions.end() || !transfer(slot->second,origins,stack,writes)) continue;
            binding(pc,origins);
            // Registration wrappers often forward a callback to another helper.
            // Follow bounded direct calls with the current argument origins;
            // their effects are discovery evidence, never folded runtime values.
            if (!i.indirect() && call_depth<4)
                pending.push_back({i.target(),origins,stack,writes,call_depth+1});
            if (conditional_call(i))
                pending.push_back({pc+8,i.likely() ? before_slot : origins,i.likely()?stack_before_slot:stack,
                    i.likely()?writes_before_slot:writes,call_depth});
            for (unsigned reg=1;reg<32;++reg)
                if (!(reg>=16 && reg<=23) && reg!=28 && reg!=29 && reg!=30) origins[reg]={};
            // Retain stack values as possible compilation candidates across a
            // call, not immutable memory. Runtime loads still read the slot.
            pending.push_back({pc+8,origins,stack,writes,call_depth});
            continue;
        }
        const auto before_slot=origins;
        const auto stack_before_slot=stack;
        const auto writes_before_slot=writes;
        const auto slot=program.instructions.find(pc+4);
        if (slot==program.instructions.end() || !transfer(slot->second,origins,stack,writes)) continue;
        binding(pc,origins);
        if (i.indirect() && i.rs==31 && !origins[2].loaded && !origins[2].offset) uses.returned|=origins[2].origin;
        if (!i.indirect()) pending.push_back({i.target(),origins,stack,writes,call_depth});
        if (i.op!=Op::j && i.op!=Op::jr)
            pending.push_back({pc+8,i.likely() ? before_slot : origins,i.likely()?stack_before_slot:stack,
                i.likely()?writes_before_slot:writes,call_depth});
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
        const auto effect=pointer_write(i);
        if (!effect) return {};
        const auto destination=*effect;
        if (destination!=reg) continue;
        if (i.op==Op::ld || i.op==Op::lq || i.op==Op::mfc1 || i.op==Op::cfc1) return {};
        if (i.op==Op::slti || i.op==Op::sltiu || i.op==Op::slt || i.op==Op::sltu) return {};
        if (i.op==Op::lui) return static_cast<std::uint32_t>(i.immediate)<<16;
        if (i.op!=Op::addiu && i.op!=Op::daddiu && i.op!=Op::ori && i.op!=Op::lw && i.op!=Op::lwu &&
            i.op!=Op::lb && i.op!=Op::lbu && i.op!=Op::lh && i.op!=Op::lhu && i.op!=Op::sll &&
            i.op!=Op::addu && i.op!=Op::daddu && i.op!=Op::bit_or) return {};
        const auto a=candidate(bytes,elf,program,before,i.rs,fuel,call_site,layout_candidate,conditional_store,follow_returns);
        if (!a) return {};
        if (i.op==Op::addiu || i.op==Op::daddiu) return *a+static_cast<std::uint32_t>(i.immediate);
        if (i.op==Op::ori) return *a|static_cast<std::uint16_t>(i.immediate);
        if (i.op==Op::lw || i.op==Op::lwu || i.op==Op::lb || i.op==Op::lbu || i.op==Op::lh || i.op==Op::lhu) {
            const auto width=i.op==Op::lb || i.op==Op::lbu ? 1U : i.op==Op::lh || i.op==Op::lhu ? 2U : 4U;
            const auto normalize=[](std::uint32_t address) {
                return address>=0x80000000U && address<0xc0000000U ? address&0x1fffffffU : address;
            };
            const auto address=normalize(*a+static_cast<std::uint32_t>(i.immediate));
            // Initial file bytes are not valid after a definite overlapping
            // write in this setup block. Recover a complete word store, or
            // leave partial/unknown replacement contents unresolved.
            for (auto prior=before;prior>=4 && fuel;) {
                --fuel; prior-=4;
                const auto previous=program.instructions.find(prior);
                if (previous==program.instructions.end()) break;
                const auto& store=previous->second;
                if (store.branch() || store.terminal() || store.op==Op::syscall) break;
                const auto extent=pointer_store_width(store.op);
                if (!extent) continue;
                const auto base=candidate(bytes,elf,program,prior,store.rs,fuel,call_site);
                if (!base) continue;
                auto at=normalize(*base+std::uint32_t(store.immediate));
                if (partial_store(store.op)) at&=~(extent-1);
                if (address-at<extent || at-address<width) {
                    if (width==4 && store.op==Op::sw && at==address)
                        return candidate(bytes,elf,program,prior,store.rt,fuel,call_site);
                    return {};
                }
            }
            if (!fuel) return {};
            return initial_word(bytes,elf,address,width,i.op==Op::lb || i.op==Op::lh);
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
struct RecordCopy { PointerLoad source; std::uint32_t destination; };
std::optional<RecordCopy> record_copy(const Program& program,const r5900::Instruction& store) {
    using r5900::Op;
    const auto at=[&](std::uint32_t pc) -> const r5900::Instruction* {
        const auto found=program.instructions.find(pc);
        return found==program.instructions.end() ? nullptr : &found->second;
    };
    if (store.pc<4 || store.rs==store.rt) return {};
    const auto* previous=at(store.pc-4);
    if (!previous) return {};
    if (store.op==Op::sd && previous->op==Op::ld && previous->rt==store.rt)
        return RecordCopy{{previous->pc,previous->rs,std::uint32_t(previous->immediate)},std::uint32_t(store.immediate)};
    if (store.pc<12 || !unaligned_pair(*previous,store,Op::sdl,Op::sdr,8)) return {};
    const auto* first=at(store.pc-12);
    const auto* second=at(store.pc-8);
    if (!first || !second || first->rs==first->rt || first->rt!=store.rt ||
        !unaligned_pair(*first,*second,Op::ldl,Op::ldr,8)) return {};
    return RecordCopy{{first->pc,first->rs,std::uint32_t(first->immediate)-(first->op==Op::ldl ? 7U : 0U)},
        std::uint32_t(store.immediate)-(store.op==Op::sdl ? 7U : 0U)};
}
std::vector<PointerLoad> called_loads(const Program& program,const pointer_flow::Graph& graph,
                                       const r5900::Instruction& call) {
    using r5900::Op;
    std::vector<PointerLoad> result;
    std::deque<std::pair<std::uint32_t,unsigned>> pending{{call.pc,call.rs}};
    std::set<std::pair<std::uint32_t,unsigned>> visited;
    unsigned budget=512;
    const auto reverse=[&](std::uint32_t pc,unsigned& reg) {
        const auto found=program.instructions.find(pc);
        if (found==program.instructions.end()) return false;
        const auto& i=found->second;
        const auto effect=pointer_write(i);
        if (!effect) return false;
        if (*effect!=reg) return true;
        if (i.op==Op::lw || i.op==Op::lwu) {
            result.push_back({i.pc,i.rs,static_cast<std::uint32_t>(i.immediate)}); return false;
        }
        const auto copy=copied_register(i);
        if (!copy || !*copy) return false;
        reg=*copy; return true;
    };
    while (!pending.empty() && budget--) {
        const auto [pc,reg]=pending.front(); pending.pop_front();
        if (!visited.emplace(pc,reg).second) continue;
        const auto found=graph.previous.find(pc);
        if (found==graph.previous.end()) continue;
        for (const auto& edge:found->second) {
            auto source=reg;
            if (edge.call && !pointer_flow::preserved(source)) continue;
            if (edge.delay && !reverse(edge.from+4,source)) continue;
            if (reverse(edge.from,source)) pending.emplace_back(edge.from,source);
        }
    }
    return result;
}
bool same_local_slot(const Program& program,const PointerLoad& load,std::uint32_t after,unsigned base,std::uint32_t offset,
                     std::uint32_t transfer=0xffffffffU) {
    using r5900::Op;
    auto before=load.pc;
    auto reg=load.base;
    auto displacement=load.offset;
    for (unsigned step=0;before>after && step<128;++step) {
        before-=4;
        const auto found=program.instructions.find(before);
        if (found==program.instructions.end()) return false;
        const auto& i=found->second;
        if (i.branch()) {
            const auto effect=pointer_write(i);
            if (before==transfer && effect && *effect!=reg) continue;
            return false;
        }
        if (i.terminal() || i.op==Op::syscall) return false;
        // A definite overwrite severs the earlier store-to-call relation.
        if (const auto width=pointer_store_width(i.op);width && i.rs==reg &&
            overwrites_word(store_range_begin(i),width,displacement)) return false;
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
// Compare local affine register aliases without assuming unknown expressions
// are equal. Used only to reject definite writes along a candidate path.
std::optional<std::uint32_t> local_alias_delta(const Program& program,std::uint32_t before,unsigned left,unsigned right) {
    std::uint32_t left_offset=0,right_offset=0;
    const auto reverse=[](const r5900::Instruction& i,unsigned& reg,std::uint32_t& offset) {
        if (const auto copy=copied_register(i)) { reg=*copy; return true; }
        if (i.op==r5900::Op::addiu || i.op==r5900::Op::daddiu) {
            reg=i.rs; offset+=std::uint32_t(i.immediate); return true;
        }
        return false;
    };
    for (unsigned step=0;step<64;++step) {
        if (left==right) return left_offset-right_offset;
        if (before<4) break;
        before-=4;
        const auto found=program.instructions.find(before);
        if (found==program.instructions.end()) break;
        const auto& i=found->second;
        if (i.branch() || i.terminal() || i.op==r5900::Op::syscall) break;
        const auto effect=pointer_write(i);
        if (!effect) break;
        if (*effect==left && !reverse(i,left,left_offset)) break;
        if (*effect==right && !reverse(i,right,right_offset)) break;
    }
    return {};
}
bool same_incoming_slot(const Program& program,const pointer_flow::Graph& graph,const PointerLoad& load,
                        std::uint32_t call_site,std::uint32_t after,unsigned base,std::uint32_t offset) {
    struct State {
        std::uint32_t pc,offset; unsigned reg;
        auto operator<=>(const State&) const=default;
    };
    std::deque<State> pending{{load.pc,load.offset,load.base}};
    std::set<State> visited;
    unsigned budget=512;
    const auto reverse=[&](std::uint32_t pc,State& state) {
        const auto found=program.instructions.find(pc);
        if (found==program.instructions.end()) return false;
        const auto& i=found->second;
        if (const auto width=pointer_store_width(i.op)) {
            const auto delta=local_alias_delta(program,pc,i.rs,state.reg);
            if (delta && overwrites_word(*delta+store_range_begin(i),width,state.offset)) return false;
        }
        const auto effect=pointer_write(i);
        if (!effect || i.terminal() || i.op==r5900::Op::syscall) return false;
        if (*effect!=state.reg) return true;
        if (const auto copy=copied_register(i)) { state.reg=*copy; return state.reg!=0; }
        if (i.op==r5900::Op::addiu || i.op==r5900::Op::daddiu) {
            state.reg=i.rs; state.offset+=std::uint32_t(i.immediate); return state.reg!=0;
        }
        return false;
    };
    const auto matches=[&](const State& state,std::uint32_t pc) {
        const auto delta=local_alias_delta(program,pc,state.reg,base);
        return delta && *delta+state.offset==offset;
    };
    while (!pending.empty() && budget--) {
        auto state=pending.front(); pending.pop_front();
        if (!visited.insert(state).second) continue;
        if (state.pc==after) {
            if (matches(state,after)) return true;
            continue;
        }
        const auto found=graph.previous.find(state.pc);
        if (found==graph.previous.end()) continue;
        for (const auto& edge:found->second) {
            auto prior=state;
            // The sink consumes arguments after its slot but before its callee.
            if (edge.call && edge.from!=call_site && !pointer_flow::preserved(prior.reg)) continue;
            // A store in a taken delay slot does not have an ordinary edge to
            // its physical successor. Match the slot before reversing its write.
            if (edge.delay && edge.from+8==after) {
                const auto slot=program.instructions.find(edge.from+4);
                if (slot!=program.instructions.end() && pointer_store_width(slot->second.op) &&
                    matches(prior,edge.from+4)) return true;
            }
            if (edge.delay && !reverse(edge.from+4,prior)) continue;
            if (!reverse(edge.from,prior)) continue;
            prior.pc=edge.from; pending.push_back(prior);
        }
    }
    return false;
}
struct CallResult { std::uint32_t pc,offset; };
// Identify a returned object without assigning it a made-up address. Follow
// copies and definite spill/reload pairs on a bounded, unambiguous predecessor
// path. Unknown definitions and merged paths remain unresolved.
std::optional<CallResult> call_result(const Program& program,const pointer_flow::Graph& graph,
                                     std::uint32_t before,unsigned reg,unsigned& budget) {
    using r5900::Op;
    std::uint32_t displacement=0;
    const auto reverse=[&](std::uint32_t pc) -> bool {
        const auto found=program.instructions.find(pc);
        if (found==program.instructions.end()) return false;
        const auto& i=found->second;
        const auto effect=pointer_write(i);
        if (!effect) return false;
        if (*effect!=reg) return true;
        if (const auto copy=copied_register(i)) { reg=*copy; return reg!=0; }
        if (i.op==Op::addiu || i.op==Op::daddiu) {
            reg=i.rs; displacement+=std::uint32_t(i.immediate); return reg!=0;
        }
        return false;
    };
    while (budget && reg) {
        --budget;
        const auto edges=graph.previous.find(before);
        if (edges==graph.previous.end() || edges->second.size()!=1) return {};
        const auto& edge=edges->second.front();
        if (edge.call && reg==2) return CallResult{edge.from,displacement};
        if (edge.call && !pointer_flow::preserved(reg)) return {};
        if (edge.delay && !reverse(edge.from+4)) return {};
        const auto& i=program.instructions.at(edge.from);
        if ((i.op==Op::lw || i.op==Op::lwu) && i.rt==reg) {
            const PointerLoad load{i.pc,i.rs,std::uint32_t(i.immediate)};
            for (auto at=i.pc;at>=4 && i.pc-at<256 && budget;) {
                --budget; at-=4;
                const auto prior=program.instructions.find(at);
                if (prior==program.instructions.end() || prior->second.branch() ||
                    prior->second.terminal() || prior->second.op==Op::syscall) break;
                const auto& store=prior->second;
                if (store.op!=Op::sw || !same_incoming_slot(program,graph,load,0xffffffffU,
                    at+4,store.rs,std::uint32_t(store.immediate))) continue;
                auto source=call_result(program,graph,at,store.rt,budget);
                if (source) source->offset+=displacement;
                return source;
            }
            return {};
        }
        if (!reverse(edge.from)) return {};
        before=edge.from;
    }
    return {};
}
// A callback descriptor must be assembled on one control-flow path. Matching
// offsets from different arms, or from a reused base register, is not evidence
// of registration. Keep this local proof bounded and respect delay-slot edges.
bool stores_one_record(const Program& program,const pointer_flow::Graph& graph,
                       const ArgumentUses::Store& function,const ArgumentUses::Store& global,
                       const ArgumentUses::Store& context) {
    const std::array fields{function,global,context};
    const auto reg=function.base_register;
    auto first=function.pc,last=function.pc;
    for (const auto& field:fields) {
        if (field.base!=function.base || field.base_register!=reg) return false;
        first=std::min(first,field.pc); last=std::max(last,field.pc);
    }
    if (last-first>256 || !reg) return false;
    using State=std::pair<std::uint32_t,unsigned>;
    std::deque<State> pending;
    if (graph.next.contains(first)) pending.emplace_back(first,0);
    if (first>=4) {
        const auto previous=graph.next.find(first-4);
        if (previous!=graph.next.end() && std::any_of(previous->second.begin(),previous->second.end(),
            [](const auto& edge) { return edge.delay; })) pending.emplace_back(first-4,0);
    }
    const auto step=[&](std::uint32_t pc,unsigned& mask) {
        const auto found=program.instructions.find(pc);
        if (found==program.instructions.end()) return false;
        const auto& i=found->second;
        for (unsigned n=0;n<fields.size();++n) if (pc==fields[n].pc) {
            mask|=1U<<n; return true;
        }
        const auto effect=pointer_write(i);
        if (!effect || (mask && *effect==reg)) return false;
        if (mask) if (const auto width=pointer_store_width(i.op)) {
            const auto delta=local_alias_delta(program,pc,i.rs,reg);
            if (delta) for (unsigned n=0;n<fields.size();++n)
                if ((mask&(1U<<n)) && overwrites_word(*delta+store_range_begin(i),width,
                    std::uint32_t(program.instructions.at(fields[n].pc).immediate)))
                    return false;
        }
        return true;
    };
    std::set<State> visited;
    unsigned budget=256;
    while (!pending.empty() && budget--) {
        const auto [pc,seen]=pending.front(); pending.pop_front();
        if (!visited.emplace(pc,seen).second) continue;
        const auto edges=graph.next.find(pc);
        if (edges==graph.next.end()) continue;
        for (const auto& edge:edges->second) {
            auto mask=seen;
            if (!step(pc,mask) || (edge.delay && !step(pc+4,mask))) continue;
            if (mask==7) return true;
            // An intervening callee could overwrite the record even when the
            // address register is ABI-preserved. Do not infer its memory effects.
            if (edge.call || edge.to<=pc || edge.to>last) continue;
            pending.emplace_back(edge.to,mask);
        }
    }
    return false;
}
struct CallConsumers {
    struct Slot {
        PointerLoad load;
        std::optional<std::pair<bool,std::uint32_t>> address;
        std::uint32_t call_site=0xffffffffU;
        bool copied_record=false;
    };
    struct Table {
        std::uint32_t base,count,stride,field,load_pc;
        std::optional<std::uint32_t> pointer_slot;
    };
    std::set<std::pair<bool,std::uint32_t>> slots; // absolute address or ABI GP offset
    std::vector<Slot> loads;
    std::vector<Table> tables;
    std::set<std::pair<std::uint32_t,std::uint32_t>> nested_fields;
    std::set<std::uint32_t> bound_fields;
    std::map<std::uint32_t,std::set<std::uint32_t>> bound_consumers;
    std::map<std::uint32_t,std::set<unsigned>> global_fields;
    std::map<std::uint32_t,std::set<std::uint32_t>> returned_fields;
    BoundCalls bindings;
    pointer_flow::Graph graph;
    static const r5900::Instruction* definition(const Program& program,std::uint32_t before,unsigned reg) {
        for (unsigned n=0;before>=4 && n<48;++n) {
            before-=4; const auto found=program.instructions.find(before);
            if (found==program.instructions.end()) break;
            const auto effect=pointer_write(found->second);
            if (!effect) break;
            if (found->second.branch()) {
                // A delay-slot store uses pre-call/pre-return registers. The
                // transfer's ABI clobbers have not happened at that point.
                if (!n && *effect!=reg) continue;
                break;
            }
            if (*effect==reg) return &found->second;
        }
        return nullptr;
    }
    static std::optional<std::pair<std::uint32_t,std::uint32_t>> indexed_layout(
            const Program& program,std::uint32_t before,unsigned reg,std::uint32_t offset=0) {
        using r5900::Op;
        for (unsigned step=0;step<8;++step) {
            const auto* i=definition(program,before,reg);
            if (!i) return {};
            if (i->op==Op::addiu || i->op==Op::daddiu) {
                offset+=std::uint32_t(i->immediate); reg=i->rs; before=i->pc;
            } else if (const auto copy=copied_register(*i)) { reg=*copy; before=i->pc; }
            else if (i->op==Op::addu || i->op==Op::daddu) {
                for (const auto index:{i->rs,i->rt}) {
                    const auto* scale=definition(program,i->pc,index);
                    if (scale && scale->op==Op::sll && scale->sa>=2 && scale->sa<=8)
                        return {{1U<<scale->sa,offset}};
                }
                return {};
            } else return {};
        }
        return {};
    }
    CallConsumers(ByteView bytes,const elf::Executable& elf,const Program& program) : graph(program) {
        using r5900::Op;
        std::set<std::pair<std::uint32_t,std::uint32_t>> bound_layouts;
        for (const auto& [pc,i]:program.instructions) {
            if (!i.indirect() || !call_transfer(i)) continue;
            for (const auto& load:called_loads(program,graph,i)) {
                unsigned fuel=128;
                const auto base=candidate(bytes,elf,program,load.pc,load.base,fuel,0xffffffffU);
                std::optional<std::pair<bool,std::uint32_t>> address;
                if (base) address={{false,physical(*base+load.offset)}};
                else if (load.base==28) address={{true,load.offset}};
                if (address) slots.insert(*address);
                loads.push_back({load,address});
                const auto slot=program.instructions.find(pc+4);
                const auto argument_loads=[&](unsigned reg) {
                    std::vector<PointerLoad> result;
                    auto argument=i; argument.rs=reg;
                    if (slot!=program.instructions.end()) {
                        const auto effect=pointer_write(slot->second);
                        if (effect && *effect==reg) {
                            if (slot->second.op==Op::lw || slot->second.op==Op::lwu)
                                result.push_back({pc+4,slot->second.rs,std::uint32_t(slot->second.immediate)});
                            else if (const auto copy=copied_register(slot->second)) {
                                argument.rs=*copy; result=called_loads(program,graph,argument);
                            }
                        } else if (effect) result=called_loads(program,graph,argument);
                    }
                    return result;
                };
                const auto same_record_field=[&](const PointerLoad& context,std::uint32_t displacement) {
                    if (context.pc>load.pc) {
                        if (same_local_slot(program,context,load.pc+4,load.base,load.offset+displacement,pc)) return true;
                        auto incoming=context;
                        if (incoming.pc==pc+4) incoming.pc=pc;
                        return incoming.pc-load.pc<=128 &&
                            same_incoming_slot(program,graph,incoming,pc,load.pc+4,load.base,load.offset+displacement);
                    }
                    return same_local_slot(program,load,context.pc+4,context.base,context.offset-displacement) ||
                        (load.pc-context.pc<=128 &&
                         same_incoming_slot(program,graph,load,pc,context.pc+4,context.base,context.offset-displacement));
                };
                const auto has_field=[&](unsigned reg,std::uint32_t displacement) {
                    const auto values=argument_loads(reg);
                    return std::any_of(values.begin(),values.end(),[&](const auto& context) {
                        return same_record_field(context,displacement);
                    });
                };
                const bool bound=has_field(4,4);
                std::set<unsigned> global_arguments;
                if (has_field(28,4)) {
                    for (unsigned reg=4;reg<12;++reg) if (has_field(reg,8)) global_arguments.insert(reg);
                }
                const bool global=!global_arguments.empty();
                if (global) global_fields[load.offset].insert(global_arguments.begin(),global_arguments.end());
                if (bound || global) {
                    if (bound) {
                        if (const auto layout=indexed_layout(program,load.pc,load.base,load.offset))
                            bound_layouts.insert(*layout);
                        bound_fields.insert(load.offset);
                    }
                    auto before=load.pc,offset=load.offset;
                    unsigned base_register=load.base;
                    for (unsigned step=0;step<8;++step) {
                        const auto* origin=definition(program,before,base_register);
                        if (!origin) break;
                        if (origin->op==r5900::Op::addiu || origin->op==r5900::Op::daddiu) {
                            offset+=std::uint32_t(origin->immediate); base_register=origin->rs;
                        } else if (const auto copy=copied_register(*origin)) base_register=*copy;
                        else break;
                        if (bound) bound_fields.insert(offset);
                        if (global) global_fields[offset].insert(global_arguments.begin(),global_arguments.end());
                        before=origin->pc;
                    }
                    if (bound) bound_consumers[offset].insert(pc);
                }
                auto parent=i; parent.pc=load.pc; parent.rs=load.base;
                for (const auto& outer:called_loads(program,graph,parent))
                    if (outer.offset<256 && load.offset<256)
                        nested_fields.emplace(outer.offset,load.offset);
            }
        }
        // Inline copies use the same complete function/context record as a
        // reference-taking helper. Only its called word is a source sink.
        for (const auto& [pc,store]:program.instructions) {
            if (store.op!=Op::sd && store.op!=Op::sdl && store.op!=Op::sdr) continue;
            const auto copy=record_copy(program,store);
            if (!copy || !bound_fields.contains(copy->destination)) continue;
            bool contiguous=true;
            for (auto at=copy->source.pc;at<pc;at+=4) {
                const auto previous=graph.previous.find(at+4);
                if (previous==graph.previous.end() || std::none_of(previous->second.begin(),previous->second.end(),
                    [&](const auto& edge) { return edge.from==at && !edge.delay && !edge.call; })) {
                    contiguous=false; break;
                }
            }
            if (contiguous) loads.push_back({copy->source,{},0xffffffffU,true});
        }
        // Indexed callback records: require an observed indirect call, a fixed
        // record stride, and a count comparison whose failure skips the load.
        // Only the called field becomes executable evidence, never the payload
        // and context fields next to it.
        const auto definition=[&](std::uint32_t before,unsigned reg) { return CallConsumers::definition(program,before,reg); };
        const auto scalar=[&](std::uint32_t pc,unsigned reg) {
            unsigned fuel=128;
            auto value=candidate(bytes,elf,program,pc,reg,fuel,0xffffffffU);
            return value ? value : pointer_flow::constant(program,graph,pc,reg);
        };
        std::map<std::uint32_t,std::set<std::uint32_t>> assigned;
        bool indexed=false;
        const auto values=[&](std::uint32_t pc,unsigned reg) {
            std::set<std::uint32_t> result;
            if (const auto value=scalar(pc,reg)) result.insert(*value);
            const auto* load=definition(pc,reg);
            if (!load || (load->op!=Op::lw && load->op!=Op::lwu)) return result;
            const auto base=scalar(load->pc,load->rs);
            if (!base) return result;
            const auto address=physical(*base+std::uint32_t(load->immediate));
            if (!indexed) {
                indexed=true;
                for (const auto& [at,store]:program.instructions) if (store.op==Op::sw) {
                    const auto destination=scalar(at,store.rs);
                    if (!destination) continue;
                    const auto value=scalar(at,store.rt);
                    if (value) assigned[physical(*destination+std::uint32_t(store.immediate))].insert(*value);
                }
            }
            if (const auto found=assigned.find(address);found!=assigned.end()) result.insert(found->second.begin(),found->second.end());
            return result;
        };
        for (const auto& entry:loads) {
            const auto& load=entry.load;
            // Fixed countdown loops walk callback records with a moving cursor.
            // Prove the same decrement on both the null-skip and call paths;
            // branch-likely delay slots must not count twice.
            for (auto at=load.pc+4;at-load.pc<=128;at+=4) {
                const auto found=program.instructions.find(at);
                if (found==program.instructions.end()) break;
                const auto& latch=found->second;
                if ((latch.op!=Op::bgez && latch.op!=Op::bgtz && latch.op!=Op::bne) ||
                    (latch.op==Op::bne && latch.rt) || latch.target()>load.pc || latch.target()+128<at ||
                    latch.rs==load.base) continue;
                const auto slot=program.instructions.find(at+4);
                if (slot==program.instructions.end()) continue;
                const auto& advance=slot->second;
                if ((advance.op!=Op::addiu && advance.op!=Op::daddiu) || advance.rt!=load.base ||
                    advance.rs!=load.base || advance.immediate<4 || advance.immediate>256 || advance.immediate%4 ||
                    load.offset>=std::uint32_t(advance.immediate)) continue;
                if (pointer_flow::path_delta(program,graph,latch.target(),at,latch.rs)!=-1 ||
                    pointer_flow::path_delta(program,graph,latch.target(),at,load.base)!=0) continue;
                const auto base=scalar(latch.target(),load.base),initial=scalar(latch.target(),latch.rs);
                if (!base || !initial || *initial>4095 || (latch.op!=Op::bgez && !*initial)) continue;
                const auto count=*initial+(latch.op==Op::bgez ? 1U : 0U);
                if (std::uint64_t(physical(*base))+std::uint64_t(count)*advance.immediate<=0x100000000ULL)
                    tables.push_back({physical(*base),count,std::uint32_t(advance.immediate),load.offset,load.pc,{}});
            }
            const auto* add=definition(load.pc,load.base);
            if (!add || (add->op!=Op::addu && add->op!=Op::daddu)) continue;
            for (unsigned side=0;side<2;++side) {
                const auto* scale=definition(add->pc,side ? add->rt : add->rs);
                if (!scale) continue;
                unsigned index=0; std::optional<std::uint32_t> stride;
                if ((scale->op==Op::mult || scale->op==Op::multu) && scale->rd) {
                    index=scale->rs; stride=scalar(scale->pc,scale->rt);
                } else if (scale->op==Op::sll && scale->sa<=8) { index=scale->rt; stride=1U<<scale->sa; }
                if (!stride || *stride<4 || *stride>256 || *stride%4 || load.offset>=*stride) continue;
                for (auto at=scale->pc;at>=4 && scale->pc-at<128;) {
                    at-=4; const auto found=program.instructions.find(at);
                    if (found==program.instructions.end()) break;
                    const auto& compare=found->second;
                    if ((compare.op!=Op::slt && compare.op!=Op::sltu) || compare.rs!=index || !compare.rd) continue;
                    const auto guard=program.instructions.find(at+4);
                    if (guard==program.instructions.end() || guard->second.op!=Op::beq ||
                        guard->second.rs!=compare.rd || guard->second.rt || guard->second.target()<=load.pc) continue;
                    bool preserved=true;
                    for (auto between=compare.pc+8;between<scale->pc;between+=4) {
                        const auto step=program.instructions.find(between);
                        const auto effect=step==program.instructions.end() ? std::nullopt : pointer_write(step->second);
                        if (!effect || *effect==index || step->second.branch()) { preserved=false; break; }
                    }
                    if (!preserved) continue;
                    const auto bases=values(add->pc,side ? add->rs : add->rt);
                    const auto counts=values(compare.pc,compare.rt);
                    std::optional<std::uint32_t> pointer_slot;
                    const auto* table_load=definition(add->pc,side ? add->rs : add->rt);
                    if (table_load && (table_load->op==Op::lw || table_load->op==Op::lwu))
                        if (const auto base=scalar(table_load->pc,table_load->rs))
                            pointer_slot=physical(*base+std::uint32_t(table_load->immediate));
                    for (auto base:bases) for (auto count:counts) {
                        base=physical(base);
                        if (base && count && count<=4096 && std::uint64_t(base)+std::uint64_t(count)* *stride<=0x100000000ULL)
                            tables.push_back({base,count,*stride,load.offset,load.pc,pointer_slot});
                    }
                }
            }
        }
        // A helper can move an object's callback into an indexed function/context
        // record. Require the setter's pair, the observed consumer layout and
        // the caller's matching destination before propagating the member use.
        std::map<std::uint32_t,ArgumentUses> summaries;
        std::map<std::uint32_t,std::uint32_t> callees;
        std::map<std::uint32_t,std::vector<std::uint32_t>> callers;
        const auto argument_register=[](unsigned origin) -> std::optional<unsigned> {
            if (!origin || (origin&(origin-1)) || origin>=256) return {};
            unsigned index=0;
            while ((1U<<index)!=origin) ++index;
            return index+4;
        };
        for (const auto& [pc,call]:program.instructions) {
            const auto callee=known_callee(bytes,elf,program,call);
            if (!callee || pc>0xfffffff7U) continue;
            callees.emplace(pc,*callee); callers[*callee].push_back(pc);
            std::array<std::optional<std::pair<std::uint32_t,std::uint32_t>>,8> layouts;
            bool has_indexed_layout=false;
            for (unsigned index=0;index<8;++index) {
                layouts[index]=indexed_layout(program,pc+8,index+4);
                has_indexed_layout|=layouts[index].has_value();
            }
            if (!has_indexed_layout) continue;
            auto found=summaries.find(*callee);
            if (found==summaries.end()) found=summaries.emplace(*callee,argument_uses(program,*callee)).first;
            const auto& uses=found->second;
            for (const auto& function:uses.stores) {
                const auto destination=argument_register(function.base),source=argument_register(function.source);
                if (!destination || !source) continue;
                const auto& layout=layouts[*destination-4];
                if (!layout || !bound_layouts.contains({layout->first,layout->second+function.offset})) continue;
                for (const auto& context:uses.stores) {
                    const auto owner=argument_register(context.source);
                    if (owner && context.base==function.base && context.offset==function.offset+4 && *owner!=*source)
                        bindings[pc].push_back({*source,*owner});
                }
            }
        }
        // A constructor may copy a function/context record into the object it
        // returns. Relate the copy to that exact return value, then propagate
        // the field through wrappers returning the same call result. Matching
        // an offset alone does not establish a factory or an object identity.
        const auto owners=[&](std::uint32_t pc) {
            std::set<std::uint32_t> result,visited;
            std::deque<std::uint32_t> pending{pc};
            unsigned budget=2048;
            while (!pending.empty() && budget--) {
                const auto at=pending.front(); pending.pop_front();
                if (!visited.insert(at).second) continue;
                if (callers.contains(at)) { result.insert(at); continue; }
                if (const auto edges=graph.previous.find(at);edges!=graph.previous.end())
                    for (const auto& edge:edges->second) pending.push_back(edge.from);
            }
            return result;
        };
        std::map<std::uint32_t,std::set<std::uint32_t>> return_owners;
        std::map<std::uint32_t,CallResult> forwarded_returns;
        for (const auto& [pc,instruction]:program.instructions) {
            if (instruction.op!=Op::jr || instruction.rs!=31 || pc>0xfffffff7U) continue;
            const auto slot=program.instructions.find(pc+4);
            if (slot==program.instructions.end()) continue;
            const auto effect=pointer_write(slot->second);
            if (!effect || *effect==2 || pointer_store_width(slot->second.op)) continue;
            unsigned fuel=256;
            if (const auto source=call_result(program,graph,pc,2,fuel))
                forwarded_returns.emplace(pc,*source);
            for (auto at=pc;at>=4 && pc-at<128;) {
                at-=4;
                const auto found=program.instructions.find(at);
                if (found==program.instructions.end() || found->second.branch() || found->second.terminal()) break;
                const auto& store=found->second;
                const auto copy=record_copy(program,store);
                if (!copy || !bound_fields.contains(copy->destination) ||
                    !same_incoming_slot(program,graph,{pc,2,copy->destination},0xffffffffU,
                        at+4,store.rs,copy->destination)) continue;
                auto owner=return_owners.find(pc);
                if (owner==return_owners.end()) owner=return_owners.emplace(pc,owners(pc)).first;
                for (const auto entry:owner->second) returned_fields[entry].insert(copy->destination);
            }
        }
        for (unsigned depth=0;depth<4;++depth) {
            bool changed=false;
            for (const auto& [pc,source]:forwarded_returns) {
                const auto callee=callees.find(source.pc);
                if (callee==callees.end()) continue;
                const auto fields=returned_fields.find(callee->second);
                if (fields==returned_fields.end()) continue;
                auto owner=return_owners.find(pc);
                if (owner==return_owners.end()) owner=return_owners.emplace(pc,owners(pc)).first;
                // Copy before inserting: recursive wrappers can share a summary.
                const auto inherited=fields->second;
                for (const auto entry:owner->second) for (const auto field:inherited)
                    changed|=returned_fields[entry].insert(field-source.offset).second;
            }
            if (!changed) break;
        }
        // Summarize only functions which can reach one of these registrations.
        // The reverse graph includes direct calls, not just their continuations.
        std::set<std::uint32_t> relevant;
        std::deque<std::uint32_t> pending;
        for (const auto& [pc,arguments]:bindings) pending.push_back(pc);
        while (!pending.empty()) {
            const auto pc=pending.front(); pending.pop_front();
            if (!relevant.insert(pc).second) continue;
            if (const auto found=graph.previous.find(pc);found!=graph.previous.end())
                for (const auto& edge:found->second) pending.push_back(edge.from);
            if (const auto found=callers.find(pc);found!=callers.end())
                pending.insert(pending.end(),found->second.begin(),found->second.end());
        }
        summaries.clear();
        for (const auto& [pc,callee]:callees) {
            if (!relevant.contains(callee) || !link_register(program.instructions.at(pc))) continue;
            auto found=summaries.find(callee);
            if (found==summaries.end()) found=summaries.emplace(callee,argument_uses(program,callee,&bindings)).first;
            for (const auto& [origin,fields]:found->second.registered_fields) {
                const auto reg=argument_register(origin);
                if (!reg) continue;
                for (const auto field:fields) loads.push_back({{pc+8,*reg,field},{},pc});
            }
        }
        // A known registered callback may invoke a second function stored in
        // its user-data object. Carry that observed field use back to the
        // caller supplying the object, rather than guessing record shapes.
        if (global_fields.empty()) return;
        std::map<std::uint32_t,ArgumentUses> callbacks;
        std::set<std::tuple<std::uint32_t,unsigned,std::uint32_t>> context_fields;
        for (const auto& [pc,callee]:callees) {
            if (!link_register(program.instructions.at(pc))) continue;
            for (unsigned index=0;index<8;++index) {
                unsigned fuel=128;
                const auto value=candidate(bytes,elf,program,pc+8,index+4,fuel,pc);
                const auto target=value ? executable_pointer(elf,*value) : std::nullopt;
                if (!target || !program.entries.contains(*target)) continue;
                auto callback=callbacks.find(*target);
                if (callback==callbacks.end()) callback=callbacks.emplace(*target,argument_uses(program,*target)).first;
                if (callback->second.callable_fields.empty()) continue;
                auto registrar=summaries.find(callee);
                if (registrar==summaries.end()) registrar=summaries.emplace(callee,argument_uses(program,callee)).first;
                const auto& uses=registrar->second;
                for (const auto& function:uses.stores) {
                    if (function.source!=(1U<<index)) continue;
                    const auto layout=global_fields.find(function.offset);
                    if (layout==global_fields.end()) continue;
                    for (const auto& global:uses.global_stores) if (global.offset==function.offset+4)
                        for (const auto& context:uses.stores) {
                            const auto reg=argument_register(context.source);
                            if (!reg || context.source==function.source || context.offset!=function.offset+8 ||
                                !stores_one_record(program,graph,function,global,context)) continue;
                            for (const auto argument:layout->second) {
                                const auto fields=callback->second.callable_fields.find(1U<<(argument-4));
                                if (fields!=callback->second.callable_fields.end())
                                    for (const auto field:fields->second) context_fields.emplace(pc,*reg,field);
                            }
                        }
                }
            }
        }
        for (const auto& [pc,reg,field]:context_fields) loads.push_back({{pc+8,reg,field},{},pc});
    }
    static std::uint32_t physical(std::uint32_t address) {
        return address>=0x80000000U && address<0xc0000000U ? address&0x1fffffffU : address;
    }
    bool indexed_store(ByteView bytes,const elf::Executable& elf,const Program& program,
                       const r5900::Instruction& store) const {
        using r5900::Op;
        if (store.op!=Op::sw || store.immediate<0) return false;
        const auto* add=definition(program,store.pc,store.rs);
        if (!add || (add->op!=Op::addu && add->op!=Op::daddu)) return false;
        // Registration helpers may receive a runtime index. Match their record
        // layout to the bounded indirect-call consumer, not to arbitrary stored
        // executable-looking arguments. The neighbouring context stays data.
        for (unsigned side=0;side<2;++side) {
            const auto* scale=definition(program,add->pc,side ? add->rt : add->rs);
            const auto* load=definition(program,add->pc,side ? add->rs : add->rt);
            if (!scale) continue;
            unsigned fuel=128;
            std::optional<std::uint32_t> stride;
            if ((scale->op==Op::mult || scale->op==Op::multu) && scale->rd)
                stride=candidate(bytes,elf,program,scale->pc,scale->rt,fuel,0xffffffffU);
            else if (scale->op==Op::sll && scale->sa<=8) stride=1U<<scale->sa;
            fuel=128;
            const auto direct=candidate(bytes,elf,program,add->pc,side ? add->rs : add->rt,fuel,0xffffffffU);
            std::optional<std::uint32_t> slot;
            if (load && (load->op==Op::lw || load->op==Op::lwu)) {
                fuel=128;
                if (const auto base=candidate(bytes,elf,program,load->pc,load->rs,fuel,0xffffffffU))
                    slot=physical(*base+std::uint32_t(load->immediate));
            }
            if (!stride) continue;
            for (const auto& table:tables)
                if (((slot && table.pointer_slot==slot) || (direct && table.base==physical(*direct))) && table.stride==*stride &&
                    table.field==std::uint32_t(store.immediate)) return true;
        }
        return false;
    }
    bool contains(ByteView bytes,const elf::Executable& elf,const Program& program,
                  std::uint32_t after,unsigned base,std::uint32_t offset,std::uint32_t call_site=0xffffffffU) const {
        unsigned fuel=128;
        auto address=candidate(bytes,elf,program,after,base,fuel,call_site);
        if (!address) address=pointer_flow::constant(program,graph,after,base);
        if (address && slots.contains({false,physical(*address+offset)})) return true;
        if (!address && base==28 && slots.contains({true,offset})) return true;
        fuel=256;
        if (const auto source=call_result(program,graph,after,base,fuel)) {
            const auto call=program.instructions.find(source->pc);
            const auto callee=call!=program.instructions.end() ? known_callee(bytes,elf,program,call->second) : std::nullopt;
            const auto fields=callee ? returned_fields.find(*callee) : returned_fields.end();
            if (fields!=returned_fields.end() && fields->second.contains(source->offset+offset)) return true;
        }
        for (const auto& entry:loads) {
            const auto& load=entry.load;
            if ((entry.call_site!=0xffffffffU || entry.copied_record) &&
                std::max(load.pc,after)-std::min(load.pc,after)<=4096 &&
                same_incoming_slot(program,graph,load,entry.call_site,after,base,offset)) return true;
            if (load.pc>=after && std::uint64_t(load.pc)<=std::uint64_t(after)+512 &&
                entry.call_site==0xffffffffU &&
                same_local_slot(program,load,after,base,offset)) return true;
        }
        if (address) for (const auto& table:tables) {
            const auto displacement=physical(*address+offset)-table.base;
            if (displacement/table.stride<table.count && displacement%table.stride==table.field) return true;
        }
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
            overwrites_word(store_range_begin(i),width,offset)) {
            if (i.op!=Op::sw || static_cast<std::uint32_t>(i.immediate)!=offset) return {};
            unsigned fuel=128;
            return candidate(bytes,elf,program,before,i.rt,fuel,call,false,call+4);
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
std::set<std::uint32_t> frame_reference_values(ByteView bytes,const elf::Executable& elf,
    const Program& program,const pointer_flow::Graph& graph,std::uint32_t call,unsigned reg,std::uint32_t displacement) {
    using r5900::Op;
    const auto slot=program.instructions.find(call+4);
    if (slot==program.instructions.end()) return {};
    const auto effect=pointer_flow::write(slot->second);
    if (!effect) return {};
    if (*effect==reg) {
        const auto& i=slot->second;
        if (i.op==Op::addiu || i.op==Op::daddiu) {
            reg=i.rs; displacement+=std::uint32_t(i.immediate);
        } else if ((i.op==Op::addu || i.op==Op::daddu || i.op==Op::bit_or) && (!i.rs || !i.rt))
            reg=i.rs ? i.rs : i.rt;
        else return {};
    }
    auto target=pointer_flow::frame_address(program,graph,call,reg);
    if (!target) return {};
    target->offset+=displacement;
    std::set<std::uint32_t> result,visited;
    std::deque<std::uint32_t> pending{call};
    unsigned budget=512;
    // Stop each path at its most recent overlapping store. A partial or unknown
    // replacement cannot resurrect an older callback value.
    const auto store=[&](std::uint32_t pc,std::uint32_t before) {
        const auto found=program.instructions.find(pc);
        if (found==program.instructions.end()) return true;
        const auto& i=found->second;
        const auto width=pointer_store_width(i.op);
        if (!width) return false;
        auto address=pointer_flow::frame_address(program,graph,before,i.rs);
        if (!address || address->root!=target->root) return false;
        address->offset+=std::uint32_t(i.immediate);
        // Partial stores may affect any byte of the aligned word/doubleword.
        if (partial_store(i.op)) address->offset&=~std::uint32_t(width-1);
        const auto delta=std::int32_t(address->offset-target->offset);
        if (std::int64_t(delta)+width<=0 || delta>=4) return false;
        if (!delta && i.op==Op::sw) {
            // A delay-slot store observes registers before the callee runs.
            // The link register itself has already changed and is not an
            // ordinary before-call value.
            if (before!=pc) if (const auto link=link_register(program.instructions.at(before));link && link==i.rt) return true;
            unsigned fuel=128;
            auto value=candidate(bytes,elf,program,pc,i.rt,fuel,before!=pc ? before : 0xffffffffU);
            if (!value) value=pointer_flow::constant(program,graph,before,i.rt);
            if (value) result.insert(*value);
        }
        return true;
    };
    if (store(call+4,call)) return result;
    while (!pending.empty() && budget--) {
        const auto pc=pending.front(); pending.pop_front();
        if (!visited.insert(pc).second) continue;
        const auto edges=graph.previous.find(pc);
        if (edges==graph.previous.end()) continue;
        for (const auto& edge:edges->second) {
            if (edge.delay && store(edge.from+4,edge.from)) continue;
            if (!store(edge.from,edge.from)) pending.push_back(edge.from);
        }
    }
    return pending.empty() ? result : std::set<std::uint32_t>{};
}
// A reference argument may point at a temporary word filled in the call delay
// slot. Recover only an unchanged SP-relative setup; other alias forms remain
// explicit unknowns rather than guesses about stack contents.
std::optional<std::uint32_t> referenced_stack_word(ByteView bytes,const elf::Executable& elf,const Program& program,
    const pointer_flow::Graph& graph,std::uint32_t call,unsigned reg,std::uint32_t offset) {
    using r5900::Op;
    const auto values=frame_reference_values(bytes,elf,program,graph,call,reg,offset);
    if (values.size()==1) return *values.begin();
    for (auto before=call+8,steps=0U;before>=4 && steps<128;++steps) {
        before-=4;
        const auto found=program.instructions.find(before);
        if (found==program.instructions.end()) return {};
        const auto& i=found->second;
        if (before==call) continue;
        if (i.branch()) {
            // We are inspecting the path that reaches this call. An ordinary
            // conditional branch whose taken arm skips it does not clobber the
            // reference on its fallthrough arm. Likely slots need separate paths.
            if (!i.indirect() && !i.likely() && !link_register(i) && i.op!=Op::j && i.target()>call) continue;
            return {};
        }
        if (i.terminal() || i.op==Op::syscall) return {};
        const auto effect=pointer_write(i);
        if (!effect || *effect==29) return {};
        if (*effect!=reg) continue;
        if (const auto copy=copied_register(i)) reg=*copy;
        else if (i.op==Op::addiu || i.op==Op::daddiu) { reg=i.rs; offset+=std::uint32_t(i.immediate); }
        else return {};
        if (reg==29) return offset%4 || offset>1024 ? std::nullopt : stack_argument(bytes,elf,program,call,offset/4);
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
        auto value=candidate(bytes,elf,program,pc,i.rt,fuel,call_site,false,pc);
        if (!value) value=pointer_flow::constant(program,consumers.graph,pc,i.rt);
        if (!value || !*value) continue;
        if (const auto address=executable_pointer(elf,*value)) {
            const bool callable=consumers.contains(bytes,elf,program,pc+4,i.rs,static_cast<std::uint32_t>(i.immediate),call_site);
            if (callable) result.insert(*address);
            if (references) references->push_back({pc,*address,false,callable,false});
        }
    }
    return result;
}
std::vector<CallbackFlowGap> callback_flow_gaps(ByteView bytes,const elf::Executable& elf,const Program& program,
                                              std::span<const PointerReference> references) {
    const CallConsumers consumers(bytes,elf,program);
    std::vector<CallbackFlowGap> result;
    std::set<std::pair<std::uint32_t,std::uint32_t>> seen;
    for (const auto& reference:references) {
        if (reference.argument || reference.callable || program.instructions.contains(reference.target) ||
            !seen.emplace(reference.site,reference.target).second) continue;
        const auto found=program.instructions.find(reference.site);
        if (found==program.instructions.end() || found->second.op!=r5900::Op::sw) continue;
        auto before=reference.site,field=std::uint32_t(found->second.immediate);
        unsigned base=found->second.rs;
        for (unsigned step=0;step<8;++step) {
            const auto* origin=CallConsumers::definition(program,before,base);
            if (!origin) break;
            if (origin->op==r5900::Op::addiu || origin->op==r5900::Op::daddiu) {
                field+=std::uint32_t(origin->immediate); base=origin->rs;
            } else if (const auto copy=copied_register(*origin)) base=*copy;
            else break;
            before=origin->pc;
        }
        const auto sites=consumers.bound_consumers.find(field);
        if (sites!=consumers.bound_consumers.end())
            result.push_back({reference.site,reference.target,field,{sites->second.begin(),sites->second.end()}});
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
        if (found==summaries.end()) found=summaries.emplace(*callee,argument_uses(program,*callee,&consumers.bindings)).first;
        const auto& uses=found->second;
        const auto argument=[&](unsigned index,bool called=false) {
            unsigned fuel=128;
            return index<8 ? candidate(bytes,elf,program,pc+8,index+4,fuel,pc,false,0,called) :
                stack_argument(bytes,elf,program,pc,index-8);
        };
        // A bound callback record contains a function and its opaque context.
        // Recognize reference-taking setters only when an actual indirect call
        // loads the adjacent context into a0, and the setter binds both fields.
        for (const auto& copy:uses.referenced_stores) {
            if (!copy.source || (copy.source&(copy.source-1)) || !consumers.bound_fields.contains(copy.offset)) continue;
            const bool context=copy.width>=8 || std::any_of(uses.stores.begin(),uses.stores.end(),[&](const auto& store) {
                return store.base==copy.base && store.offset==copy.offset+4 && store.source && store.source!=copy.source;
            });
            if (!context) continue;
            unsigned index=0;
            while (index<8 && (1U<<index)!=copy.source) ++index;
            if (index==8) continue;
            const auto value=referenced_stack_word(bytes,elf,program,consumers.graph,pc,index+4,copy.source_offset);
            const auto entry=value ? executable_pointer(elf,*value) : std::nullopt;
            if (!entry) continue;
            result.insert(*entry);
            if (references) references->push_back({pc,*entry,true,true,false,*callee,index});
        }
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
                // A saved-GP callback ABI binds the function, GP and opaque
                // argument together. Only match a layout observed at an actual
                // indirect invocation, with all three writes on one path.
                if (!callable && consumers.global_fields.contains(store.offset))
                    for (const auto& global:uses.global_stores) if (global.offset==store.offset+4)
                        for (const auto& context:uses.stores)
                            if (context.source!=store.source && context.offset==store.offset+8 &&
                                stores_one_record(program,consumers.graph,store,global,context)) callable=true;
                const auto instruction=program.instructions.find(store.pc);
                if (instruction!=program.instructions.end() && consumers.indexed_store(bytes,elf,program,instruction->second))
                    callable=true;
                // A helper can attach an object to a completion record and
                // install the object's callback in the same operation. Match
                // both links to an observed nested indirect call; a neighbouring
                // opaque field is not a callback merely because it is stored.
                if (store.base && !(store.base&(store.base-1)))
                    for (const auto& outer:uses.stores)
                        if (outer.source==store.base && outer.pc!=store.pc &&
                            consumers.nested_fields.contains({outer.offset,store.offset})) callable=true;
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
    const pointer_flow::Graph graph(program);
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
        const auto frame_entries=frame_reference_values(bytes,elf,program,graph,pc,4,4);
        for (const auto value:frame_entries)
            if (const auto entry=executable_pointer(elf,value)) result.insert(*entry);
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
