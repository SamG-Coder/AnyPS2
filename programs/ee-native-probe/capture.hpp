#pragma once
#include <nt/ee_native.hpp>
#include <nt/binary.hpp>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <chrono>

namespace probe {
// A bounded tail avoids changing startup timing with synchronous per-call I/O.
// Records are observations only and cannot be restored as executable state.
class ServiceCapture {
public:
    struct Record {
        std::uint32_t pc,number,thread,status,pending,completion;
        std::array<std::uint32_t,4> arguments;
        std::uint32_t return_address=0;
        std::uint64_t nanoseconds=0;
    };
    void observe(Record record) { record.nanoseconds=elapsed(); records_[count_++%records_.size()]=record; }
    struct Delivery { bool dma; unsigned line; std::uint32_t pc,control; std::uint64_t nanoseconds=0; };
    void deliver(Delivery delivery) { delivery.nanoseconds=elapsed(); deliveries_[delivered_++%deliveries_.size()]=delivery; }
    void save(const std::filesystem::path& directory) const {
        std::ofstream output(directory/"services.tsv");
        output.exceptions(std::ios::failbit|std::ios::badbit);
        output<<"index\tpc\tsyscall\tthread\tstatus\tpending\tcompletion\ta0\ta1\ta2\ta3\treturn_address\tnanoseconds\n";
        const auto start=count_>records_.size() ? count_-records_.size() : 0;
        for (auto i=start;i<count_;++i) {
            const auto& r=records_[i%records_.size()];
            output<<std::dec<<i<<std::hex<<'\t'<<r.pc<<'\t'<<r.number<<'\t'<<r.thread
                  <<'\t'<<r.status<<'\t'<<r.pending<<'\t'<<r.completion;
            for (const auto argument:r.arguments) output<<'\t'<<argument;
            output<<'\t'<<r.return_address<<std::dec<<'\t'<<r.nanoseconds<<'\n';
        }
        std::ofstream interrupts(directory/"deliveries.tsv");
        interrupts.exceptions(std::ios::failbit|std::ios::badbit);
        interrupts<<"index\tdma\tline\tpc\tcontrol\tnanoseconds\n";
        const auto begin=delivered_>deliveries_.size() ? delivered_-deliveries_.size() : 0;
        for (auto i=begin;i<delivered_;++i) {
            const auto& d=deliveries_[i%deliveries_.size()];
            interrupts<<std::dec<<i<<'\t'<<d.dma<<'\t'<<d.line<<std::hex<<'\t'<<d.pc<<'\t'<<d.control<<std::dec<<'\t'<<d.nanoseconds<<'\n';
        }
    }
private:
    const std::chrono::steady_clock::time_point epoch_=std::chrono::steady_clock::now();
    std::uint64_t elapsed() const {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-epoch_).count();
    }
    // Keep enough opt-in history to compare several animation phases without
    // synchronous tracing I/O. Storage stays bounded even for long sessions.
    std::vector<Record> records_=std::vector<Record>(65536);
    std::uint64_t count_=0;
    std::vector<Delivery> deliveries_=std::vector<Delivery>(4096);
    std::uint64_t delivered_=0;
};
// Optional IOP observation shares the private stop directory. An idle scheduler
// has no active continuation; do not invent a CPU snapshot in that case.
template<class Cpu>
void capture_iop(const std::filesystem::path& directory,std::span<const std::uint8_t> ram,const Cpu* state) {
    nt::write_file(directory/"iop-ram.bin",ram);
    std::ofstream out(directory/"iop-cpu.txt");
    out.exceptions(std::ios::failbit|std::ios::badbit);
    out<<"format=anyps2-iop-observation-v1\nresumable=false\nactive="<<(state!=nullptr)<<'\n';
    if (state) {
        out<<"pc="<<state->pc<<"\nexecuted="<<state->executed<<"\nbranch_target="<<state->branch_target
           <<"\ndelay_slot="<<state->delay_slot<<"\nload_register="<<state->load_register<<"\nload_value="<<state->load_value
           <<"\nstatus="<<state->status<<"\ncause="<<state->cause<<"\nepc="<<state->epc
           <<"\ninterrupt_depth="<<state->interrupt_depth<<"\nhi="<<state->hi<<"\nlo="<<state->lo<<'\n';
        for (unsigned index=0;index<state->registers.size();++index) out<<"gpr"<<index<<'='<<state->registers[index]<<'\n';
    }
    out.close();
}
template<class Pixel>
void capture_image(const std::filesystem::path& path,unsigned width,unsigned height,Pixel pixel) {
    std::ofstream image(path,std::ios::binary);
    image.exceptions(std::ios::failbit|std::ios::badbit);
    image<<"P6\n"<<width<<' '<<height<<"\n255\n";
    for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x) {
        const auto value=pixel(x,y);
        const char rgb[]{static_cast<char>(value),static_cast<char>(value>>8),static_cast<char>(value>>16)};
        image.write(rgb,3);
    }
    image.close();
}
// Diagnostic CPU/memory observation, not a resumable runtime save state.
// The manifest is written last so partial captures cannot appear complete.
inline void capture_stop(const std::filesystem::path& directory,
                         const nt::runtime::State& state,
                         const nt::ee::Memory& memory,nt::ee::Stop stop,std::uint64_t input_identity=0) {
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
       <<"\nsa="<<state.sa<<"\nfcr31="<<state.fcr31<<"\naccumulator="<<state.accumulator<<"\naccumulator_overflow="<<state.accumulator_overflow
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
    manifest<<"format=anyps2-stop-observation-v2\ncomplete=true\nresumable=false\nram_bytes="
            <<memory.size<<"\nscratchpad_bytes="<<memory.scratchpad_bytes
            <<"\ninput_identity="<<input_identity<<"\nactive_image="<<memory.executable_image<<'\n';
    manifest.close();
}
}
