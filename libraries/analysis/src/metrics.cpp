#include <nt/metrics.hpp>
#include <array>
#include <stdexcept>

namespace nt::analysis {
namespace {
using r5900::Op;
struct Value { bool dependent=false; std::optional<std::uint32_t> constant; };
// Candidate addresses within straight-line regions. Reset at every direct
// target and after a branch, and discard constants on unknown operations.
// This deliberately does not infer constants across calls or indirect entries.
struct MemoryCandidates {
    std::vector<MetricMemoryUse> references;
    std::vector<MetricPointerUse> pointers;
};
MemoryCandidates memory_candidates(const Program& program) {
    std::set<std::uint32_t> targets;
    for (const auto& [pc,i]:program.instructions)
        if (i.branch() && !i.indirect()) targets.insert(i.target());
    std::array<std::optional<std::uint32_t>,32> values{};
    const auto reset=[&]() { values.fill({}); values[0]=0; };
    reset();
    MemoryCandidates result;
    std::optional<std::uint32_t> previous;
    std::optional<r5900::Instruction> handoff;
    for (const auto& [pc,i]:program.instructions) {
        if (!previous || std::uint64_t(*previous)+4!=pc || targets.contains(pc)) { reset(); handoff.reset(); }
        previous=pc;
        if (i.branch()) {
            if (!handoff && (i.op==Op::j || i.op==Op::jal || i.op==Op::jr || i.op==Op::jalr)) {
                handoff=i;
                // The link is overwritten before the delay slot executes.
                if (i.op==Op::jal) values[31].reset();
                if (i.op==Op::jalr && i.rd) values[i.rd].reset();
            } else { reset(); handoff.reset(); }
            continue;
        }
        unsigned destination=0,width=0;
        bool write=false;
        std::optional<std::uint32_t> next;
        const auto a=values[i.rs],b=values[i.rt];
        switch (i.op) {
        case Op::lui: destination=i.rt; next=std::uint32_t(i.immediate)<<16; break;
        case Op::addiu: case Op::daddiu:
            destination=i.rt; if (a) next=*a+std::uint32_t(i.immediate); break;
        case Op::ori: case Op::andi: case Op::xori:
            destination=i.rt;
            if (a) next=i.op==Op::ori ? *a|std::uint16_t(i.immediate) :
                i.op==Op::andi ? *a&std::uint16_t(i.immediate) : *a^std::uint16_t(i.immediate);
            break;
        case Op::addu: case Op::daddu: case Op::subu: case Op::dsubu: case Op::bit_or:
            destination=i.rd;
            if (a && b) next=i.op==Op::bit_or ? *a|*b :
                (i.op==Op::subu || i.op==Op::dsubu) ? *a-*b : *a+*b;
            break;
        case Op::sll: case Op::srl:
            destination=i.rd; if (b) next=i.op==Op::sll ? *b<<i.sa : *b>>i.sa; break;
        case Op::lb: case Op::lbu: width=1; destination=i.rt; break;
        case Op::lh: case Op::lhu: width=2; destination=i.rt; break;
        case Op::lw: case Op::lwu: width=4; destination=i.rt; break;
        case Op::ld: width=8; destination=i.rt; break;
        case Op::sb: width=1; write=true; break;
        case Op::sh: width=2; write=true; break;
        case Op::sw: width=4; write=true; break;
        case Op::sd: width=8; write=true; break;
        case Op::mfc0: destination=i.rt; break;
        case Op::mtc0: case Op::sync: case Op::cache: case Op::pref: break;
        default: reset(); break;
        }
        if (width && a) result.references.push_back({pc,*a+std::uint32_t(i.immediate),width,write,*a});
        if (write && width>=4 && b)
            result.pointers.push_back({pc,*b,i.rt,MetricPointerKind::stored_pointer,{}});
        if (destination) values[destination]=next;
        if (handoff) {
            const bool returning=handoff->op==Op::jr && handoff->rs==31;
            const unsigned first=returning ? 2 : 4,last=returning ? 4 : 12;
            for (unsigned reg=first;reg<last;++reg) if (values[reg])
                result.pointers.push_back({handoff->pc,*values[reg],reg,
                    returning ? MetricPointerKind::return_value : MetricPointerKind::call_argument,
                    handoff->indirect() ? std::nullopt : std::optional(handoff->target())});
            reset(); handoff.reset();
        }
    }
    return result;
}
std::optional<r5900::Instruction> instruction_at(ByteView bytes,const elf::Executable& elf,std::uint32_t pc) {
    for (const auto& segment:elf.segments)
        if ((segment.flags&1) && pc>=segment.address &&
            std::uint64_t(pc)+4<=std::uint64_t(segment.address)+segment.file_size)
            return r5900::decode(pc,le32(bytes,std::size_t(segment.offset)+pc-segment.address));
    return {};
}
void follow(ByteView bytes,const elf::Executable& elf,MetricSite& site,std::size_t window) {
    std::array<Value,32> values{};
    values[0].constant=0;
    if (!site.reg) return;
    values[site.reg].dependent=true;
    auto pc=site.pc;
    for (std::size_t step=0;step<window;++step) {
        if (pc>0xfffffffbU) { site.uses.push_back({pc,MetricUseKind::missing_code,{}}); return; }
        pc+=4;
        const auto decoded=instruction_at(bytes,elf,pc);
        if (!decoded) { site.uses.push_back({pc,MetricUseKind::missing_code,{}}); return; }
        const auto& i=*decoded;
        if (i.op==Op::unsupported) {
            site.uses.push_back({pc,MetricUseKind::unknown_operation,{}}); return;
        }
        // Stop before branches, including their delay slots. A caller/CFG
        // analysis must resolve both paths before drawing semantic conclusions.
        if (i.branch() || i.terminal() || i.op==Op::syscall) {
            site.uses.push_back({pc,MetricUseKind::flow_boundary,{}}); return;
        }
        unsigned destination=0;
        Value next;
        const auto source=values[i.rs],target=values[i.rt];
        const auto offset=static_cast<std::uint32_t>(i.immediate);
        const auto address=source.constant ? std::optional<std::uint32_t>(*source.constant+offset) : std::nullopt;
        const auto memory=[&]() {
            if (source.dependent) site.uses.push_back({pc,MetricUseKind::memory_address,{}});
        };
        switch (i.op) {
        case Op::sync: case Op::cache: case Op::pref: break;
        case Op::lui: destination=i.rt; next.constant=std::uint32_t(i.immediate)<<16; break;
        case Op::addiu: case Op::daddiu:
            destination=i.rt; next={source.dependent,address}; break;
        case Op::ori: case Op::andi: case Op::xori:
            destination=i.rt; next.dependent=source.dependent;
            if (source.constant) next.constant=i.op==Op::ori ? *source.constant|std::uint16_t(i.immediate) :
                i.op==Op::andi ? *source.constant&std::uint16_t(i.immediate) : *source.constant^std::uint16_t(i.immediate);
            break;
        case Op::addu: case Op::daddu: case Op::subu: case Op::dsubu:
        case Op::bit_or: case Op::bit_and: case Op::bit_xor: case Op::bit_nor:
        case Op::slt: case Op::sltu:
            destination=i.rd; next.dependent=source.dependent || target.dependent;
            if (!i.rt && (i.op==Op::addu || i.op==Op::daddu || i.op==Op::bit_or)) next.constant=source.constant;
            else if (!i.rs && (i.op==Op::addu || i.op==Op::daddu || i.op==Op::bit_or)) next.constant=target.constant;
            break;
        case Op::sll: case Op::srl: case Op::sra:
            destination=i.rd; next.dependent=target.dependent;
            if (target.constant && i.op==Op::sll) next.constant=*target.constant<<i.sa;
            if (target.constant && i.op==Op::srl) next.constant=*target.constant>>i.sa;
            break;
        case Op::sb: case Op::sh: case Op::sw: case Op::sd: case Op::sq:
            memory();
            if (target.dependent) site.uses.push_back({pc,MetricUseKind::stored_value,address});
            break;
        case Op::lb: case Op::lbu: case Op::lh: case Op::lhu: case Op::lw: case Op::lwu: case Op::ld:
            memory(); destination=i.rt; next.dependent=source.dependent; break;
        case Op::mfc0: destination=i.rt; break;
        case Op::mtc0:
            if (target.dependent) site.uses.push_back({pc,MetricUseKind::system_write,{}});
            break;
        default: site.uses.push_back({pc,MetricUseKind::unknown_operation,{}}); return;
        }
        if (destination) values[destination]=next;
        bool live=false;
        for (const auto& value:values) live|=value.dependent;
        if (!live) return;
    }
    site.uses.push_back({pc,MetricUseKind::budget,{}});
}
}
std::vector<MetricSite> inspect_metrics(ByteView bytes,const elf::Executable& elf,std::size_t window) {
    if (!window || window>4096) throw std::invalid_argument("Metric dependency window must be 1..4096 instructions");
    std::vector<MetricSite> result;
    Program candidates;
    // Validate every segment before following instructions into another one.
    for (const auto& segment:elf.segments)
        if ((segment.flags&1) && (segment.address%4 ||
            std::uint64_t(segment.offset)+segment.file_size>bytes.size() ||
            std::uint64_t(segment.address)+segment.file_size>0x100000000ULL))
            throw std::invalid_argument("Invalid executable segment for metric inspection");
    for (const auto& segment:elf.segments) {
        if (!(segment.flags&1)) continue;
        for (std::uint64_t offset=0;offset+4<=segment.file_size;offset+=4) {
            const auto i=r5900::decode(segment.address+static_cast<std::uint32_t>(offset),le32(bytes,segment.offset+std::size_t(offset)));
            candidates.instructions.emplace(i.pc,i);
            if ((i.op!=Op::mfc0 && i.op!=Op::mtc0) || i.rd!=25) continue;
            const auto selector=i.word&63U;
            MetricSite site{i.pc,i.op==Op::mtc0,(selector&1)!=0,(selector>>1)&1,i.rt,{}, {}, {}};
            if (!site.write && site.counter) follow(bytes,elf,site,window);
            result.push_back(std::move(site));
        }
    }
    const auto memory=memory_candidates(candidates);
    for (auto& site:result) {
        std::set<std::uint32_t> seen;
        std::set<std::pair<std::uint32_t,unsigned>> pointer_seen;
        for (auto& use:site.uses) {
            if (use.kind!=MetricUseKind::stored_value) continue;
            const MetricMemoryUse* store=nullptr;
            for (const auto& candidate:memory.references) if (candidate.pc==use.pc) { store=&candidate; break; }
            if (!store) continue;
            use.address=store->address;
            for (const auto& candidate:memory.references)
                if (std::uint64_t(candidate.address)<std::uint64_t(store->address)+store->bytes &&
                    std::uint64_t(store->address)<std::uint64_t(candidate.address)+candidate.bytes && seen.insert(candidate.pc).second)
                    site.memory_uses.push_back(candidate);
            for (const auto& pointer:memory.pointers)
                if ((pointer.value==store->base || pointer.value==store->address) &&
                    pointer_seen.emplace(pointer.pc,pointer.reg).second)
                    site.pointer_uses.push_back(pointer);
        }
    }
    return result;
}
void describe_metrics(std::ostream& out,std::span<const MetricSite> sites) {
    out<<"Performance-transfer candidates: "<<sites.size()<<" (executable-segment scan; reachability unproven)\n";
    out<<"Register dependencies stop at flow/unknown boundaries; memory consumers remain unresolved. No removal or replacement is authorized.\n";
    out<<"Memory references are syntactic constant-address candidates; dynamic aliases and indirect entries remain unresolved.\n";
    constexpr const char* names[]{"stored-value","memory-address","system-write","flow-boundary","unknown-operation","missing-code","budget"};
    for (const auto& site:sites) {
        out<<hex32(site.pc)<<' '<<(site.write ? "write " : "read ")<<(site.counter ? "counter" : "control")
           <<" index="<<site.index<<" register="<<site.reg<<'\n';
        for (const auto& use:site.uses) {
            out<<"  "<<hex32(use.pc)<<' '<<names[static_cast<unsigned>(use.kind)];
            if (use.address) out<<" address="<<hex32(*use.address);
            out<<'\n';
        }
        for (const auto& use:site.memory_uses)
            out<<"  memory-candidate "<<hex32(use.pc)<<' '<<(use.write ? "write" : "read")
               <<" address="<<hex32(use.address)<<" bytes="<<use.bytes<<'\n';
        constexpr const char* pointer_names[]{"call-argument","return-value","stored-pointer"};
        for (const auto& use:site.pointer_uses) {
            out<<"  pointer-candidate "<<hex32(use.pc)<<' '<<pointer_names[static_cast<unsigned>(use.kind)]
               <<" value="<<hex32(use.value)<<" register="<<use.reg;
            if (use.target) out<<" target="<<hex32(*use.target);
            out<<'\n';
        }
    }
}
}
