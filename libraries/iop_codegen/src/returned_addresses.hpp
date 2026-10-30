#pragma once
#include "constants.hpp"

namespace nt::iop_codegen::ir {
// Enumerate constant results of bounded selectors without merging their
// distinct switch arms. These are compilation candidates, not permission to
// execute a pointer or to replace the selector with a native service.
inline std::set<std::uint32_t> returned_constants(const Program& program,std::uint32_t entry) {
    struct Path { std::uint32_t pc; Constants state; std::set<std::uint32_t> visited; };
    std::vector<Path> pending{{entry,{},{}}};
    std::set<std::uint32_t> results;
    unsigned steps=0;
    while (!pending.empty()) {
        auto path=std::move(pending.back()); pending.pop_back();
        while (true) {
            if (++steps>4096 || pending.size()>512 || !path.visited.insert(path.pc).second) return {};
            const auto at=program.instructions.find(path.pc);
            if (at==program.instructions.end()) return {};
            const auto& instruction=at->second;
            auto state=transfer(instruction,path.state);
            if (instruction.flow==Flow::sequential) {
                path.pc+=4; path.state=std::move(state); continue;
            }
            if (instruction.flow==Flow::call || instruction.flow==Flow::indirect_call) {
                // A call may clobber all tracked values. Only constants defined
                // again on its return continuation can contribute a result.
                path.pc+=8; path.state=Constants{}; continue;
            }
            if (instruction.flow!=Flow::direct && instruction.flow!=Flow::conditional &&
                instruction.flow!=Flow::indirect && instruction.flow!=Flow::return_register) return {};
            const auto delay=program.instructions.find(path.pc+4);
            if (delay==program.instructions.end() || delay->second.flow!=Flow::sequential) return {};
            state=transfer(delay->second,std::move(state));
            if (instruction.flow==Flow::return_register) {
                if (!state.registers[2]) return {};
                results.insert(state.registers[2]->begin(),state.registers[2]->end());
                if (results.size()>256) return {};
                break;
            }
            if (instruction.flow==Flow::indirect) {
                if (instruction.targets.empty()) return {};
                for (const auto target : instruction.targets) pending.push_back({target,state,path.visited});
                break;
            }
            if (instruction.flow==Flow::conditional) pending.push_back({path.pc+8,state,path.visited});
            path.pc=instruction.target; path.state=std::move(state);
        }
    }
    return results;
}
}
