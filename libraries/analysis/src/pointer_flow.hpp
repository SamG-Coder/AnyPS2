#pragma once
#include <nt/analysis.hpp>
#include <deque>
#include <functional>
#include <tuple>

namespace nt::analysis::pointer_flow {
// This needs no propagated register value: either comparison reads the same
// register twice, so its taken edge is certain even for nonzero registers.
inline bool always_taken(const r5900::Instruction& i) {
    return (i.op==r5900::Op::beq || i.op==r5900::Op::beql) && i.rs==i.rt;
}
// The low word can carry a pointer. Conditional and partial writes invalidate it
// too. No value semantics are inferred for operations listed only by effect.
inline std::optional<unsigned> write(const r5900::Instruction& i) {
    using r5900::Op;
    switch (i.op) {
    case Op::unsupported: case Op::syscall: return {};
    case Op::jal: case Op::bltzal: case Op::bgezal: case Op::bltzall: case Op::bgezall: return 31;
    case Op::lui: case Op::addi: case Op::addiu: case Op::daddi: case Op::daddiu:
    case Op::slti: case Op::sltiu: case Op::andi: case Op::ori: case Op::xori:
    case Op::lb: case Op::lbu: case Op::lh: case Op::lhu: case Op::lw: case Op::lwu:
    case Op::ld: case Op::lq: case Op::lwl: case Op::lwr: case Op::ldl: case Op::ldr:
    case Op::mfc0: case Op::mfc1: case Op::cfc1: case Op::cfc2: case Op::qmfc2: return i.rt;
    case Op::jalr: case Op::sll: case Op::srl: case Op::sra: case Op::sllv: case Op::srlv: case Op::srav:
    case Op::dsll: case Op::dsrl: case Op::dsra: case Op::dsll32: case Op::dsrl32: case Op::dsra32:
    case Op::dsllv: case Op::dsrlv: case Op::dsrav: case Op::movz: case Op::movn:
    case Op::add: case Op::sub: case Op::addu: case Op::subu: case Op::dadd: case Op::dsub:
    case Op::daddu: case Op::dsubu: case Op::bit_and: case Op::bit_or: case Op::bit_xor: case Op::bit_nor:
    case Op::slt: case Op::sltu: case Op::mfhi: case Op::mflo: case Op::mfhi1: case Op::mflo1: case Op::mfsa:
    case Op::mult: case Op::multu: case Op::mult1: case Op::multu1:
    case Op::madd: case Op::maddu: case Op::madd1: case Op::maddu1: case Op::plzcw:
    case Op::pextlb: case Op::pextlh: case Op::pextlw: case Op::pextub: case Op::pextuh: case Op::pextuw:
    case Op::pand: case Op::por: case Op::pxor: case Op::pnor: case Op::padduw:
    case Op::pceqb: case Op::pceqh: case Op::pceqw: case Op::ppac5: case Op::pext5:
    case Op::ppacb: case Op::ppach: case Op::ppacw: case Op::pcpyh: case Op::pcpyld: case Op::pcpyud:
    case Op::paddw: case Op::psubw: case Op::pcgtw: case Op::pmaxw: case Op::pminw: case Op::pabsw:
    case Op::paddsw: case Op::psubsw: case Op::psubuw: case Op::paddh: case Op::psubh:
    case Op::pcgth: case Op::pmaxh: case Op::pminh: case Op::pabsh: case Op::paddsh:
    case Op::psubsh: case Op::padduh: case Op::psubuh: case Op::paddb: case Op::psubb:
    case Op::pcgtb: case Op::paddsb: case Op::psubsb: case Op::paddub: case Op::psubub:
    case Op::padsbh: case Op::psllvw: case Op::psrlvw: case Op::psravw:
    case Op::psllh: case Op::psrlh: case Op::psrah: case Op::psllw: case Op::psrlw: case Op::psraw:
    case Op::pexeh: case Op::prevh: case Op::pexew: case Op::prot3w: case Op::pexch: case Op::pexcw:
    case Op::pinth: case Op::pinteh: case Op::qfsrv: case Op::pmfhi: case Op::pmflo:
    case Op::pmultw: case Op::pmultuw: case Op::pmadduw: case Op::pmfhl:
    case Op::pmulth: case Op::pmaddh: case Op::pmsubh: case Op::phmadh: case Op::phmsbh: return i.rd;
    case Op::sw: case Op::sd: case Op::sq: case Op::sb: case Op::sh:
    case Op::swl: case Op::swr: case Op::sdl: case Op::sdr: case Op::swc1: case Op::sqc2:
    case Op::sync: case Op::pref: case Op::cache: case Op::breakpoint: case Op::eret:
    case Op::jr: case Op::j: case Op::beq: case Op::bne: case Op::blez: case Op::bgtz:
    case Op::bltz: case Op::bgez: case Op::beql: case Op::bnel: case Op::blezl: case Op::bgtzl:
    case Op::bltzl: case Op::bgezl: case Op::bc1f: case Op::bc1t: case Op::bc1fl: case Op::bc1tl:
    case Op::bc0f: case Op::bc0t: case Op::bc0fl: case Op::bc0tl:
    case Op::mthi: case Op::mtlo: case Op::mthi1: case Op::mtlo1: case Op::mtsa: case Op::mtsab: case Op::mtsah:
    case Op::div: case Op::divu: case Op::div1: case Op::divu1: case Op::pdivw: case Op::pdivuw: case Op::pdivbw:
    case Op::pmthi: case Op::pmtlo: case Op::pmthl:
    case Op::mtc0: case Op::mtc1: case Op::ctc1: case Op::ctc2: case Op::qmtc2: case Op::lwc1: case Op::lqc2:
    case Op::ei: case Op::di: case Op::tlbr: case Op::tlbwi: case Op::tlbwr: case Op::tlbp:
    case Op::tge: case Op::tgeu: case Op::tlt: case Op::tltu: case Op::teq: case Op::tne:
    case Op::tgei: case Op::tgeiu: case Op::tlti: case Op::tltiu: case Op::teqi: case Op::tnei:
    case Op::mov_s: case Op::abs_s: case Op::neg_s: case Op::mul_s: case Op::div_s:
    case Op::add_s: case Op::sub_s: case Op::max_s: case Op::min_s: case Op::cvt_s_w: case Op::cvt_w_s:
    case Op::compare_s: case Op::adda_s: case Op::suba_s: case Op::mula_s: case Op::sqrt_s:
    case Op::rsqrt_s: case Op::madd_s: case Op::msub_s: case Op::madda_s: case Op::msuba_s:
    case Op::vadd_q: case Op::vsub_q: case Op::vadd_i: case Op::vsub_i:
    case Op::vsub: case Op::vadd: case Op::vsub_bc: case Op::vadd_bc: case Op::vmove: case Op::vmr32:
    case Op::vmul: case Op::vmul_bc: case Op::vmax: case Op::vmax_bc: case Op::vmini: case Op::vmini_bc:
    case Op::vabs: case Op::vnop: case Op::vitof0: case Op::vitof4: case Op::vitof12: case Op::vitof15:
    case Op::vftoi0: case Op::vftoi4: case Op::vftoi12: case Op::vftoi15: case Op::vcallms: case Op::vcallmsr:
    case Op::vmula_bc: case Op::vmula: case Op::vmadd: case Op::vmsub: case Op::vmadd_bc: case Op::vmsub_bc:
    case Op::vmadda: case Op::vmsuba: case Op::vmadda_bc: case Op::vmsuba_bc:
    case Op::vdiv: case Op::vsqrt: case Op::vrsqrt: case Op::vwaitq: case Op::vclip:
    case Op::vopmula: case Op::vopmsub: case Op::vrnext: case Op::vrget: case Op::vrinit: case Op::vrxor:
    case Op::vadda: case Op::vsuba: case Op::vadda_bc: case Op::vsuba_bc: case Op::vmul_q: case Op::vmul_i:
    case Op::vmadd_q: case Op::vmadd_i: case Op::vmsub_q: case Op::vmsub_i: case Op::vmula_q: case Op::vmula_i: case Op::vadda_q: case Op::vadda_i: case Op::vsuba_q: case Op::vsuba_i: case Op::vmadda_q: case Op::vmadda_i: case Op::vmsuba_q: case Op::vmsuba_i: case Op::vmfir: case Op::vmtir: case Op::vlqi: case Op::vsqi: case Op::vlqd: case Op::vsqd: case Op::vilwr: case Op::viswr: return 0;
    }
    return {};
}
inline bool preserved(unsigned reg) { return (reg>=16 && reg<=23) || reg==28 || reg==29 || reg==30; }
struct FrameAddress {
    std::uint32_t root,offset;
    auto operator<=>(const FrameAddress&) const=default;
};
struct Edge {
    std::uint32_t from,to;
    bool delay=false,call=false;
};
// Edges execute the instruction at `from`, then its delay slot when selected,
// and finally apply a call's ABI clobbers. Likely-false edges omit the slot.
struct Graph {
    std::map<std::uint32_t,std::vector<Edge>> next,previous;
    // Addresses also entered by a call or as a discovery root. The edges above
    // describe local flow only, so register values there are unknown even when a
    // tail jump is the one recorded predecessor.
    std::set<std::uint32_t> external_entries;
    explicit Graph(const Program& program,const std::map<std::uint32_t,std::set<std::uint32_t>>& candidate_edges={}) {
        using r5900::Op;
        const auto add=[&](Edge edge) {
            if (!program.instructions.contains(edge.to)) return;
            next[edge.from].push_back(edge); previous[edge.to].push_back(edge);
        };
        for (const auto& [pc,i]:program.instructions) {
            if ((!program.ordinary.empty() && !program.ordinary.contains(pc)) || pc>0xfffffff7U ||
                i.terminal() || i.op==Op::syscall) continue;
            if (!i.branch()) { add({pc,pc+4}); continue; }
            const bool link=i.op==Op::jal || (i.op==Op::jalr && i.rd) || (i.op>=Op::bltzal && i.op<=Op::bgezall);
            const bool always=always_taken(i);
            const bool never=(i.op==Op::bne || i.op==Op::bnel) && i.rs==i.rt;
            if (link) {
                add({pc,pc+8,true,true});
                if (!i.indirect()) external_entries.insert(i.target());
            }
            else if (!i.indirect() && !never) add({pc,i.target(),true});
            else if (i.op==Op::jr && i.rs!=31) {
                if (const auto targets=candidate_edges.find(pc);targets!=candidate_edges.end())
                    for (const auto target:targets->second) add({pc,target,true});
            }
            if (!always && i.op!=Op::j && i.op!=Op::jal && i.op!=Op::jr && i.op!=Op::jalr)
                add({pc,pc+8,!i.likely()});
        }
        // Independent entries can expose otherwise disconnected instructions.
        // After pruning a constant edge, those instructions must not act as
        // fresh roots in a backward slice unless reachable from an entry.
        external_entries.insert(program.entries.begin(),program.entries.end());
        if (!program.entries.empty()) {
            std::deque<std::uint32_t> pending(program.entries.begin(),program.entries.end());
            std::set<std::uint32_t> reached;
            while (!pending.empty()) {
                const auto pc=pending.front(); pending.pop_front();
                if (!reached.insert(pc).second) continue;
                const auto instruction=program.instructions.find(pc);
                if (instruction==program.instructions.end()) continue;
                const auto& i=instruction->second;
                if (i.op==Op::jal || (i.op>=Op::bltzal && i.op<=Op::bgezall)) pending.push_back(i.target());
                if (const auto edges=next.find(pc);edges!=next.end())
                    for (const auto& edge:edges->second) pending.push_back(edge.to);
            }
            for (auto& [pc,edges]:previous)
                std::erase_if(edges,[&](const auto& edge) { return !reached.contains(edge.from); });
        }
    }
};
// Recover switch-table candidates from a range predicate. A masked comparison
// establishes the low-index subset, not an upper bound on the unmasked runtime
// index. This is discovery evidence only; execution retains the guard and load.
inline std::optional<std::uint32_t> switch_candidate_bound(const Program& program,const Graph& graph,
                                               std::uint32_t before,unsigned guard,unsigned index) {
    if (!guard || !index || guard==index) return {};
    std::deque<std::uint32_t> pending{before};
    std::set<std::uint32_t> visited;
    std::optional<std::uint32_t> result;
    unsigned budget=1024;
    const auto reverse=[&](std::uint32_t pc) {
        const auto found=program.instructions.find(pc);
        if (found==program.instructions.end()) return -1;
        const auto& i=found->second;
        const auto effect=write(i);
        if (!effect || i.terminal()) return -1;
        if (*effect!=guard && *effect!=index) return 0;
        if (i.op!=r5900::Op::sltiu || i.rt!=guard || i.immediate<=0 || i.immediate>4096) return -1;
        const auto bound=static_cast<std::uint32_t>(i.immediate);
        if (i.rs!=index) {
            bool matched=false;
            for (auto at=i.pc,steps=0U;at>=4 && steps<32;++steps) {
                at-=4;
                const auto found=program.instructions.find(at);
                if (found==program.instructions.end()) break;
                const auto& mask=found->second;
                const auto effect=write(mask);
                if (!effect || mask.branch() || mask.terminal() || *effect==index) break;
                if (*effect!=i.rs) continue;
                const auto bits=std::uint32_t(std::uint16_t(mask.immediate));
                matched=mask.op==r5900::Op::andi && mask.rs==index && bits &&
                    !(bits&(bits+1)) && bound<=bits+1;
                break;
            }
            if (!matched) return -1;
        }
        if (result && *result!=bound) return -1;
        result=bound; return 1;
    };
    while (!pending.empty() && budget--) {
        const auto pc=pending.front(); pending.pop_front();
        if (!visited.insert(pc).second) continue;
        const auto edges=graph.previous.find(pc);
        if (edges==graph.previous.end() || edges->second.empty()) return {};
        for (const auto& edge:edges->second) {
            if (edge.call && (!preserved(guard) || !preserved(index))) return {};
            if (edge.delay) {
                const auto delay=reverse(edge.from+4);
                if (delay<0) return {};
                if (delay>0) continue;
            }
            const auto step=reverse(edge.from);
            if (step<0) return {};
            if (!step) pending.push_back(edge.from);
        }
    }
    return pending.empty() ? result : std::nullopt;
}
// Recover affine stack aliases relative to one function entry. Requiring the
// same root and offset on every incoming path prevents unrelated frames from
// being equated, while ABI-preserved aliases can survive intervening calls.
inline std::optional<FrameAddress> frame_address(const Program& program,const Graph& graph,
                                                std::uint32_t before,unsigned reg,unsigned budget=512) {
    using r5900::Op;
    struct State {
        std::uint32_t pc,offset; unsigned reg;
        auto operator<=>(const State&) const=default;
    };
    std::deque<State> pending{{before,0,reg}};
    std::set<State> visited;
    std::optional<FrameAddress> result;
    const auto reverse=[&](std::uint32_t pc,State& state) {
        const auto found=program.instructions.find(pc);
        if (found==program.instructions.end()) return false;
        const auto& i=found->second;
        const auto effect=write(i);
        if (!effect || i.terminal()) return false;
        if (*effect!=state.reg) return true;
        if (i.op==Op::addiu || i.op==Op::daddiu) {
            state.reg=i.rs; state.offset+=std::uint32_t(i.immediate); return state.reg!=0;
        }
        if ((i.op==Op::addu || i.op==Op::daddu || i.op==Op::bit_or) && (!i.rs || !i.rt)) {
            state.reg=i.rs ? i.rs : i.rt; return state.reg!=0;
        }
        return false;
    };
    while (!pending.empty() && budget--) {
        auto state=pending.front(); pending.pop_front();
        if (!visited.insert(state).second) continue;
        const auto edges=graph.previous.find(state.pc);
        if (edges==graph.previous.end() || edges->second.empty()) {
            if (state.reg!=29) return {};
            const FrameAddress value{state.pc,state.offset};
            if (result && *result!=value) return {};
            result=value; continue;
        }
        for (const auto& edge:edges->second) {
            auto next=state;
            if (edge.call && !preserved(next.reg)) return {};
            if (edge.delay && !reverse(edge.from+4,next)) return {};
            if (!reverse(edge.from,next)) return {};
            next.pc=edge.from; pending.push_back(next);
        }
    }
    return pending.empty() ? result : std::nullopt;
}
// Recover one agreed scalar constant across predecessor blocks. Path states
// stay separate; a clobbered or unknown incoming value makes the result unknown.
inline std::optional<std::uint64_t> constant_gpr(const Program& program,const Graph& graph,
    std::uint32_t before,unsigned reg,
    const std::function<std::optional<std::uint32_t>(const r5900::Instruction&)>& stack_word={}) {
    using r5900::Op;
    struct Operation {
        Op op; std::uint64_t value;
        auto operator<=>(const Operation&) const=default;
    };
    struct State {
        std::uint32_t pc; unsigned reg; std::vector<Operation> operations;
        auto operator<=>(const State&) const=default;
    };
    std::deque<State> pending{{before,reg,{}}};
    std::set<State> visited;
    std::optional<std::uint64_t> result;
    bool unknown=false;
    unsigned budget=512;
    const auto word=[](std::uint64_t value) {
        const auto low=value&0xffffffffULL;
        return (low^0x80000000ULL)-0x80000000ULL;
    };
    const auto accept=[&](const State& state,std::uint64_t value) {
        for (auto op=state.operations.rbegin();op!=state.operations.rend();++op) {
            if (op->op==Op::ori) value|=op->value;
            else if (op->op==Op::addiu) value=word(value+op->value);
            else if (op->op==Op::sll) value=word(value);
            else value+=op->value; // DADDIU wraps at 64 bits.
        }
        if (result && *result!=value) unknown=true;
        result=value;
    };
    const auto reverse=[&](std::uint32_t pc,State& state) {
        const auto found=program.instructions.find(pc);
        if (found==program.instructions.end()) { unknown=true; return false; }
        const auto& i=found->second;
        const auto effect=write(i);
        if (!effect || i.terminal()) { unknown=true; return false; }
        if (*effect!=state.reg) return true;
        if (i.op==Op::lui) { accept(state,word(std::uint32_t(i.immediate)<<16)); return false; }
        if (i.op==Op::addiu || i.op==Op::daddiu || i.op==Op::ori) {
            state.operations.push_back({i.op,i.op==Op::ori ? std::uint64_t(std::uint16_t(i.immediate)) :
                std::uint64_t(std::int64_t(i.immediate))});
            state.reg=i.rs; return true;
        }
        if ((i.op==Op::addu || i.op==Op::daddu || i.op==Op::bit_or) && (!i.rs || !i.rt)) {
            if (i.op==Op::addu) state.operations.push_back({Op::sll,0});
            state.reg=i.rs ? i.rs : i.rt; return true;
        }
        if (i.op==Op::sll && !i.sa) { state.operations.push_back({Op::sll,0}); state.reg=i.rt; return true; }
        if (stack_word && (i.op==Op::lw || i.op==Op::lwu)) {
            if (const auto value=stack_word(i)) {
                accept(state,i.op==Op::lw ? word(*value) : std::uint64_t(*value)); return false;
            }
        }
        unknown=true; return false;
    };
    while (!pending.empty() && budget-- && !unknown) {
        auto state=std::move(pending.front()); pending.pop_front();
        if (!state.reg) { accept(state,0); continue; }
        if (!visited.insert(state).second) continue;
        if (graph.external_entries.contains(state.pc)) { unknown=true; break; }
        const auto edges=graph.previous.find(state.pc);
        if (edges==graph.previous.end()) { unknown=true; break; }
        for (const auto& edge:edges->second) {
            auto next=state;
            if (edge.call && !preserved(next.reg)) { unknown=true; break; }
            if (edge.delay && !reverse(edge.from+4,next)) continue;
            if (!next.reg) { accept(next,0); continue; }
            if (reverse(edge.from,next)) { next.pc=edge.from; pending.push_back(std::move(next)); }
        }
    }
    return unknown || !pending.empty() ? std::nullopt : result;
}
inline std::optional<std::uint32_t> constant(const Program& program,const Graph& graph,
                                            std::uint32_t before,unsigned reg) {
    const auto value=constant_gpr(program,graph,before,reg);
    return value ? std::optional{static_cast<std::uint32_t>(*value)} : std::nullopt;
}
struct AffineIndex {
    std::uint32_t scale=0,offset=0;
    auto operator<=>(const AffineIndex&) const=default;
};
// Recover a low-word address expression relative to an index at a range guard.
// Every incoming path must agree. The bounded slice follows scheduled copies
// and earlier switch edges but rejects calls, cycles and unknown definitions.
inline std::optional<AffineIndex> guarded_address(const Program& program,const Graph& graph,
    std::uint32_t before,unsigned reg,std::uint32_t guard,unsigned index) {
    using r5900::Op;
    using Key=std::pair<std::uint32_t,unsigned>;
    std::map<Key,std::optional<AffineIndex>> cache;
    std::set<Key> active;
    unsigned fuel=512;
    std::function<std::optional<AffineIndex>(std::uint32_t,unsigned)> read;
    const auto step=[&](const r5900::Instruction& i,unsigned target,std::uint32_t input)->std::optional<AffineIndex> {
        const auto effect=write(i);
        if (!effect || i.terminal() || i.op==Op::syscall) return {};
        if (*effect!=target) return read(input,target);
        if (i.op==Op::lui) return AffineIndex{0,std::uint32_t(i.immediate)<<16};
        if (i.op==Op::addiu || i.op==Op::daddiu) {
            auto value=read(input,i.rs);
            if (value) value->offset+=std::uint32_t(i.immediate);
            return value;
        }
        if ((i.op==Op::addu || i.op==Op::daddu || i.op==Op::bit_or) && (!i.rs || !i.rt))
            return read(input,i.rs ? i.rs : i.rt);
        if (i.op==Op::addu || i.op==Op::daddu) {
            const auto left=read(input,i.rs),right=read(input,i.rt);
            if (left && right) return AffineIndex{left->scale+right->scale,left->offset+right->offset};
        }
        if (i.op==Op::sll && i.sa<=8) {
            const auto value=read(input,i.rt);
            if (value) return AffineIndex{value->scale<<i.sa,value->offset<<i.sa};
        }
        if (i.op==Op::ori) {
            const auto value=read(input,i.rs);
            if (value && !value->scale) return AffineIndex{0,value->offset|std::uint16_t(i.immediate)};
        }
        return {};
    };
    read=[&](std::uint32_t pc,unsigned target)->std::optional<AffineIndex> {
        if (!target) return AffineIndex{};
        if (pc==guard) {
            if (target==index) return AffineIndex{1,0};
            const auto value=constant(program,graph,pc,target);
            return value ? std::optional{AffineIndex{0,*value}} : std::nullopt;
        }
        const Key key{pc,target};
        if (const auto found=cache.find(key);found!=cache.end()) return found->second;
        if (!fuel || active.contains(key)) return {};
        --fuel; active.insert(key);
        std::optional<AffineIndex> result;
        bool valid=true;
        const auto incoming=graph.previous.find(pc);
        if (incoming==graph.previous.end() || incoming->second.empty()) valid=false;
        else for (const auto& edge:incoming->second) {
            if (edge.call) { valid=false; break; }
            const auto from=program.instructions.find(edge.from);
            if (from==program.instructions.end()) { valid=false; break; }
            std::optional<AffineIndex> value;
            if (edge.delay) {
                const auto slot=program.instructions.find(edge.from+4);
                const auto effect=write(from->second);
                // Ordinary conditional/indirect branches do not write GPRs.
                if (!effect || *effect || slot==program.instructions.end()) { valid=false; break; }
                value=step(slot->second,target,edge.from);
            } else value=step(from->second,target,edge.from);
            if (!value || (result && *result!=*value)) { valid=false; break; }
            result=value;
        }
        active.erase(key);
        if (!valid) result.reset();
        cache.emplace(key,result); return result;
    };
    return read(before,reg);
}
// Prove one affine update on every forward path through a bounded loop body.
// Nested loops, unknown writes and volatile values across calls are rejected.
inline std::optional<std::int64_t> path_delta(const Program& program,const Graph& graph,
    std::uint32_t first,std::uint32_t last,unsigned reg) {
    using r5900::Op;
    std::deque<std::pair<std::uint32_t,std::int64_t>> pending{{first,0}};
    std::set<std::pair<std::uint32_t,std::int64_t>> visited;
    std::optional<std::int64_t> result;
    const auto step=[&](std::uint32_t pc,std::int64_t& delta) {
        const auto found=program.instructions.find(pc);
        if (found==program.instructions.end()) return false;
        const auto& i=found->second;
        const auto effect=write(i);
        if (!effect) return false;
        if (*effect!=reg) return true;
        if ((i.op!=Op::addiu && i.op!=Op::daddiu) || i.rs!=reg) return false;
        delta+=i.immediate; return true;
    };
    unsigned budget=256;
    while (!pending.empty() && budget--) {
        const auto [pc,value]=pending.front(); pending.pop_front();
        if (!visited.emplace(pc,value).second) continue;
        if (pc==last) {
            if (result && *result!=value) return {};
            result=value; continue;
        }
        const auto edges=graph.next.find(pc);
        if (edges==graph.next.end() || edges->second.empty()) return {};
        for (const auto& edge:edges->second) {
            auto delta=value;
            if (edge.to<=pc || edge.to>last || (edge.call && !preserved(reg)) ||
                !step(pc,delta) || (edge.delay && !step(pc+4,delta))) return {};
            pending.emplace_back(edge.to,delta);
        }
    }
    return pending.empty() ? result : std::nullopt;
}
}
