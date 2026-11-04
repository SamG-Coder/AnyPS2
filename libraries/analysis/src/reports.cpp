#include <nt/analysis.hpp>
#include <ostream>

namespace nt::analysis {
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
