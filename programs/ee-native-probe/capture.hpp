#pragma once
#include <nt/ee_native.hpp>
#include <nt/binary.hpp>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace probe {
// Diagnostic CPU/memory observation, not a resumable runtime save state.
// The manifest is written last so partial captures cannot appear complete.
inline void capture_stop(const std::filesystem::path& directory,
                         const nt::runtime::State& state,
                         const nt::ee::Memory& memory,nt::ee::Stop stop) {
    if (!std::filesystem::create_directory(directory))
        throw std::runtime_error("Stop capture directory already exists");
    nt::write_file(directory/"ram.bin",{memory.data,static_cast<std::size_t>(memory.size)});
    nt::write_file(directory/"scratchpad.bin",{memory.scratchpad,static_cast<std::size_t>(memory.scratchpad_bytes)});
    std::ofstream out(directory/"cpu.txt");
    out.exceptions(std::ios::failbit|std::ios::badbit);
    out<<"format=anyps2-cpu-observation-v1\nstop="<<static_cast<unsigned>(stop)
       <<"\npc="<<state.pc<<"\nexecuted="<<state.executed
       <<"\ndelay_slot="<<state.delay_slot<<"\nbranch_pc="<<state.branch_pc
       <<"\npending_target="<<state.pending_target
       <<"\ncop0_status="<<state.cop0_status<<"\ncop0_epc="<<state.cop0_epc
       <<"\ncop0_error_epc="<<state.cop0_error_epc
       <<"\nhi="<<state.hi<<"\nlo="<<state.lo<<"\nhi1="<<state.hi1<<"\nlo1="<<state.lo1
       <<"\nsa="<<state.sa<<"\nfcr31="<<state.fcr31<<"\naccumulator="<<state.accumulator
       <<"\nperformance_control="<<memory.performance.control
       <<"\nunavailable_counters="<<static_cast<unsigned>(memory.metrics.unavailable_counters)
       <<"\nperformance0="<<memory.performance.counters[0]
       <<"\nperformance1="<<memory.performance.counters[1]
       <<"\nfault_address="<<memory.fault_address<<"\npending_interrupts="<<memory.pending_interrupts<<'\n';
    for (unsigned i=0;i<state.gpr.size();++i)
        out<<"gpr"<<i<<"="<<state.gpr[i].lo<<","<<state.gpr[i].hi<<'\n';
    for (unsigned i=0;i<state.fpr.size();++i) out<<"fpr"<<i<<"="<<state.fpr[i]<<'\n';
    out.close();
    std::ofstream manifest(directory/"capture.txt");
    manifest.exceptions(std::ios::failbit|std::ios::badbit);
    manifest<<"format=anyps2-stop-observation-v1\ncomplete=true\nresumable=false\nram_bytes="
            <<memory.size<<"\nscratchpad_bytes="<<memory.scratchpad_bytes<<'\n';
    manifest.close();
}
}
