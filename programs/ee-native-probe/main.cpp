#include "capture.hpp"
#include "graphics_capture.hpp"
#include "vector_capture.hpp"
#include "packet_capture.hpp"
#include <cstdlib>
#include <nt/host_isa.hpp>
#include <nt/ee_services.hpp>
#include <nt/ee_platform.hpp>
#include <nt/ee_sif_command_peer.hpp>
#include <nt/binary.hpp>
#include <nt/ee_file_rpc.hpp>
#include <nt/ee_cdvd_rpc.hpp>
#include <nt/ee_bootstrap_modules.hpp>
#include <nt/ee_pad_rpc.hpp>
#include <nt/pad_script.hpp>
#include <nt/ee_audsrv_rpc.hpp>
#include <nt/ee_performance.hpp>
#include <nt/live_counters.hpp>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <memory>
#include <optional>
#include "display.hpp"
#include "input_latency.hpp"
#include <nt/vulkan_renderer.hpp>
#include <nt/native_drawing_worker.hpp>
#include "thread_sampler.hpp"
#include <charconv>
#include <cstdio>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <thread>
#ifdef ANYPS2_NATIVE_IOP
#include "iop_reset.hpp"
#endif
#if defined(ANYPS2_NATIVE_VU) || defined(ANYPS2_NATIVE_VU1)
#include <nt/vu_artifact.hpp>
#ifdef ANYPS2_NATIVE_VU1_REGIONS
#include <nt/vector_region.hpp>
#include <atomic>
#include <bitset>
#include <map>
#include <mutex>
#include <tuple>
extern "C" const void* nt_vu1_regions();
// Diagnostic: a VU module converted with --observe-loads reports each data load.
// Rows written by the most recent executed region; later loads that hit them
// are reads of region output by ordinary VU code (not the graphics kick).
namespace {
std::bitset<1024> region_rows;
unsigned region_rows_head=0;
std::map<unsigned,std::uint64_t> region_row_reads,region_row_executions;
std::uint64_t observed_loads=0;
}
extern "C" void nt_vu_observe_load(std::uint32_t row) {
    ++observed_loads;
    if (row<1024 && region_rows[row]) {
        ++region_row_reads[region_rows_head];
        region_rows.reset(); // count each execution's outputs once
    }
}
extern "C" std::uint32_t nt_vu1_region_count();
namespace {
// Converter-emitted vector regions of the linked VU1 image, by loop head.
// check: validate each entry with the region guard but run the original code.
// cpu: execute accepted entries with the region service (exact, transactional).
struct VuRegions {
    struct Entry { std::uint32_t head; const nt::render::VectorRegion* region; };
    std::array<const nt::render::VectorRegion*,2048> by_head{};
    bool execute=false;
    std::atomic<std::uint64_t> entries{0},accepted{0},rejected{0},executed{0},failed{0};
    std::array<std::atomic<std::uint64_t>,5> settled{}; // accepted after 0..4 settling iterations
    // Pass chains (executed regions only; the VU1 worker is the only caller):
    // consecutive region heads, and whether the later one writes the first
    // output row the earlier one wrote (an in-place pass over its output).
    unsigned last_head=0; std::int64_t last_row=-1;
    std::map<std::tuple<unsigned,unsigned,bool>,std::uint64_t> transitions;
    std::int64_t first_output_row(const nt::render::VectorRegion& region,const nt::vu::State& s) const {
        const auto& store=region.output[0];
        return std::int64_t(s.integers[store.base-1])+store.row;
    }
    std::mutex reasons_mutex;
    std::map<std::string,std::uint64_t> reasons; // Rejection reasons, at most 32 distinct.
    void reject(const std::exception& error) {
        std::lock_guard lock(reasons_mutex);
        if (reasons.size()<32 || reasons.contains(error.what())) ++reasons[error.what()];
    }
    std::shared_ptr<nt::vu::LoopSubstitutes> substitutes(bool run_regions) {
        execute=run_regions;
        auto result=std::make_shared<nt::vu::LoopSubstitutes>();
        result->context=this; result->heads.assign(2048,false);
        const auto* table=static_cast<const Entry*>(nt_vu1_regions());
        for (std::uint32_t i=0;i<nt_vu1_region_count();++i) {
            if (table[i].head>=2048 || !table[i].region) throw std::runtime_error("Invalid linked VU1 region table");
            by_head[table[i].head]=table[i].region; result->heads[table[i].head]=true;
        }
        result->run=[](void* raw,unsigned head,nt::vu::State& state) {
            auto& self=*static_cast<VuRegions*>(raw);
            ++self.entries;
            const auto& region=*self.by_head[head];
            if (!self.execute) {
                // Diagnostic: settle on a copy with up to four original iterations
                // while pipelines are busy at the head, then validate.
                auto copy=state;
                for (unsigned settle=0;;++settle) {
                    try { (void)nt::render::validate_vector_region(region,copy); ++self.accepted; ++self.settled[std::min(settle,4U)]; break; }
                    catch (const std::exception& error) {
                        const bool busy=std::string_view(error.what()).find("quiescent")!=std::string_view::npos;
                        if (!busy || settle==4 || region.iteration(&copy)!=nt::vu::Result::transfer || copy.next_pair!=region.head) {
                            ++self.rejected; self.reject(error); break;
                        }
                    }
                }
                return false;
            }
            const auto row=self.first_output_row(region,state);
            const auto entry=state;
            try {
                nt::render::execute_vector_region(region,state); ++self.executed;
                // Rows every element of every store wrote (the guard bounded them).
                region_rows.reset(); region_rows_head=head; ++region_row_executions[head];
                const unsigned count=entry.integers[region.counter-1]/region.step+region.trip_offset;
                for (unsigned n=0;n<region.stores;++n) for (unsigned element=0;element<count;++element) {
                    const auto& store=region.output[n];
                    const auto value=std::int64_t(entry.integers[store.base-1])+store.row+std::int64_t(store.stride)*element;
                    if (value>=0 && value<1024) region_rows.set(std::size_t(value));
                }
                if (self.transitions.size()<256) ++self.transitions[{self.last_head,head,row==self.last_row}];
                self.last_head=head; self.last_row=row;
                return true;
            }
            catch (const std::exception& error) { ++self.failed; self.reject(error); return false; }
        };
        return result;
    }
};
}
#endif
#include <nt/ee_vu_upload.hpp>
#include <nt/ee_vu_launch.hpp>
#endif
#ifdef ANYPS2_NATIVE_VU1
// Diagnostic: raw entry states of VU1 launches after a start time, for local
// replay of converted VU programs. Captures contain game data; keep them in
// ignored local directories.
struct VuJobCapture {
    std::ofstream file;
    std::uint64_t remaining=0;
    std::chrono::steady_clock::time_point begin;
    static void observe(void* context,unsigned unit,const nt::vu::State& state) {
        auto& self=*static_cast<VuJobCapture*>(context);
        if (unit!=1 || !self.remaining || std::chrono::steady_clock::now()<self.begin) return;
        self.file.write(reinterpret_cast<const char*>(&state),sizeof(state));
        if (!--self.remaining) self.file.flush();
    }
};
#endif
extern "C" nt::ee::Stop nt_ee_generated_run(nt::runtime::State*,nt::ee::Memory*,std::uint64_t);
extern "C" bool nt_ee_generated_active_code_overlap(std::uint32_t,std::uint32_t,std::uint32_t);
extern "C" nt::ee::Stop nt_ee_generated_publish_image(nt::ee::Memory*,std::uint32_t);
extern "C" bool nt_ee_generated_contains(std::uint32_t);
extern "C" std::uint64_t nt_ee_generated_input_identity();
namespace {
nt::ee::Memory* active_program_memory=nullptr;
bool native_code_overlap(std::uint32_t address,std::uint32_t bytes) {
    return nt_ee_generated_active_code_overlap(address,bytes,active_program_memory ? active_program_memory->executable_image : 1);
}
struct VuCapture {
    unsigned unit=0,destination=0;
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint8_t> packet;
    static void observe_packet(void* context,unsigned,std::span<const std::uint8_t> data) {
        auto& self=*static_cast<VuCapture*>(context);
        if (self.packet.empty()) self.packet.assign(data.begin(),data.end());
    }
    static void observe(void* context,unsigned unit,unsigned destination,std::span<const std::uint8_t> data) {
        auto& self=*static_cast<VuCapture*>(context);
        if (!self.bytes.empty()) return;
        self.unit=unit; self.destination=destination; self.bytes.assign(data.begin(),data.end());
    }
    void save(const std::filesystem::path& directory) const {
        if (bytes.empty()) return;
        if (!std::filesystem::create_directory(directory)) throw std::runtime_error("VU capture directory already exists");
        nt::write_file(directory/"upload.bin",bytes);
        if (!packet.empty()) nt::write_file(directory/"packet.bin",packet);
        std::ofstream metadata(directory/"upload.txt");
        if (!(metadata<<"unit="<<unit<<"\ndestination="<<destination<<"\ninstructions="<<bytes.size()/8
                      <<"\nfingerprint="<<nt::fingerprint(bytes)<<"\n")) throw std::runtime_error("Cannot write VU capture metadata");
    }
};
struct NativeGpu {
    std::unique_ptr<nt::graphics::VulkanRenderer> headless_renderer;
    nt::graphics::VulkanRenderer* renderer=nullptr;
    // GPU primitive assembly is the default where the renderer supports it.
    bool verify_textures=false,profile_commands=false,gpu_triangles=true,gpu_triangles_required=false,gpu_triangles_enabled=false;
    nt::graphics::NativeDrawing drawing{true,nt::graphics::NativeDrawing::Completion::recorded};
    nt::graphics::NativeDrawing::Profile drawing_profile{};
    std::unique_ptr<nt::graphics::NativeDrawingWorker> worker;
    // Render thread only: the sub-pixel offset jitter a game keeps applying.
    // A frame that draws nothing (a repeated image) does not end it; a game
    // that stops jittering loses the presentation filter after a second.
    struct JitterHold {
        unsigned jitter=0,quiet=0;
        std::uint32_t smoothing(unsigned observed,bool half_height) {
            if (observed) { jitter=observed; quiet=0; }
            else if (jitter && ++quiet>=60) jitter=0;
            return nt::graphics::jitter_smoothing(jitter,half_height);
        }
    } jitter_hold;
    std::uint64_t reported_finishes=0;
    void ensure_renderer() {
        if (!renderer) {
            headless_renderer=std::make_unique<nt::graphics::VulkanRenderer>();
            renderer=headless_renderer.get();
            renderer->verify_texture_kernels(verify_textures);
            renderer->profile_commands(profile_commands);
        }
        // Enabled before the worker owns the drawing; the worker thread is its only user afterwards.
        if (gpu_triangles && !gpu_triangles_enabled && !worker) {
            if (drawing.assemble_triangles_on_gpu(*renderer)) gpu_triangles_enabled=true;
            else if (gpu_triangles_required)
                throw std::runtime_error("--gpu-triangles requires the native Vulkan renderer with ordered fragment access");
            else {
                std::cerr<<"graphics: GPU primitive assembly unavailable (no ordered fragment access); using CPU planning\n";
                gpu_triangles=false;
            }
        }
        if (!worker) worker=std::make_unique<nt::graphics::NativeDrawingWorker>(drawing,*renderer);
    }
    std::unique_ptr<probe::GraphicsCapture> capture;
    nt::ee::NativeGraphics* completion=nullptr;
    std::string error;
    std::chrono::steady_clock::duration render_time{},drain_time{};
    void join() {
        if (worker) { worker->wait(); render_time=worker->elapsed(); }
    }
    static bool submit(void* raw,std::span<const nt::graphics::NativeDrawing::Packet> packets) {
        auto& self=*static_cast<NativeGpu*>(raw);
        try {
            self.ensure_renderer();
            if (self.capture) self.capture->submit(packets);
            self.worker->submit(packets);
            self.render_time=self.worker->elapsed();
            return true;
        }
        catch (const std::exception& error) { self.error=error.what(); return false; }
    }
    static bool take(void* raw,std::vector<nt::graphics::Packet>& packets) {
        auto& self=*static_cast<NativeGpu*>(raw);
        try {
            self.ensure_renderer();
            if (self.capture) self.capture->submit(packets);
            self.worker->submit(std::move(packets));
            self.render_time=self.worker->elapsed();
            return true;
        }
        catch (const std::exception& error) { self.error=error.what(); return false; }
    }
    // VIF FLUSH boundary: the render queue already orders later commands
    // after earlier ones, and presentation, FINISH and readbacks submit the
    // recorded Vulkan work, so a FLUSH needs neither a host wait nor its own
    // GPU submission. Data the program reads back joins the queue explicitly.
    static bool fence(void* raw) {
        auto& self=*static_cast<NativeGpu*>(raw);
        try { self.ensure_renderer(); return true; }
        catch (const std::exception& error) { self.error=error.what(); return false; }
    }
    static nt::ee::Stop poll(void* context) {
        auto& self=*static_cast<NativeGpu*>(context);
        if (!self.error.empty()) return nt::ee::Stop::unsupported_system;
        if (!self.renderer) return nt::ee::Stop::budget;
        try {
            if (self.worker && !self.worker->poll()) return nt::ee::Stop::budget;
            if (self.worker) self.render_time=self.worker->elapsed();
            if (self.renderer->validation_errors()) {
                self.error=self.renderer->validation_message();
                return nt::ee::Stop::unsupported_system;
            }
            self.drawing.poll_completions(*self.renderer);
            if (self.drawing.completed_finishes()!=self.reported_finishes && self.completion) self.completion->drawing_completed();
            self.reported_finishes=self.drawing.completed_finishes();
            return nt::ee::Stop::budget;
        } catch (const std::exception& error) { self.error=error.what(); return nt::ee::Stop::unsupported_system; }
    }
    static nt::ee::Stop synchronize(void* context) {
        auto& self=*static_cast<NativeGpu*>(context);
        try {
            self.ensure_renderer();
            self.join(); self.renderer->synchronize();
            return poll(context);
        } catch (const std::exception& error) {
            self.error=error.what(); return nt::ee::Stop::unsupported_system;
        }
    }
};
bool trace_services=false;
probe::ServiceCapture* service_capture=nullptr;
nt::ee::StartupServices* active_services=nullptr;
#ifdef ANYPS2_NATIVE_IOP
probe::IopReset* active_iop=nullptr;
bool profile_iop=false;
std::uint64_t iop_time_ns=0;
#endif
nt::ee::Stop execute(nt::runtime::State* state,nt::ee::Memory* memory,std::uint64_t budget) {
#ifdef ANYPS2_NATIVE_IOP
    if (active_iop) {
        const auto began=profile_iop ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const auto result=active_iop->pump();
        if (profile_iop) iop_time_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-began).count();
        if (result!=nt::ee::Stop::budget) return result;
    }
#endif
    const auto stop=nt_ee_generated_run(state,memory,budget);
    if (service_capture && stop==nt::ee::Stop::syscall) {
        const auto thread=active_services->current_thread();
        probe::ServiceCapture::Record record{state->pc,static_cast<std::uint32_t>(state->gpr[3].lo),
            thread ? thread->id : 0,state->cop0_status,memory->pending_interrupts,
            memory->completions ? memory->completions->status() : 0,{}};
        for (unsigned i=0;i<4;++i) record.arguments[i]=static_cast<std::uint32_t>(state->gpr[4+i].lo);
        record.return_address=static_cast<std::uint32_t>(state->gpr[31].lo);
        service_capture->observe(record);
    }
    if (trace_services && stop==nt::ee::Stop::syscall) {
        std::cerr<<"service pc="<<nt::hex32(state->pc)<<" number="<<state->gpr[3].lo;
        for (unsigned i=4;i<8;++i) std::cerr<<" a"<<(i-4)<<"="<<nt::hex32(static_cast<std::uint32_t>(state->gpr[i].lo));
        std::cerr<<'\n';
    }
    return stop;
}
}
int main(int argc,char** argv) {
    if (argc<2) {
        std::cerr<<"Usage: ps2-native <program.data> [operation-budget] [options]\n"
                   "  Play: --disc <image.iso> --disc-media cd|dvd --memory-card <folder> --fullscreen --resolution-scale 1|3|5 --close-on-stop --display-stop --run-seconds N --headless\n"
                   "  Input: --pad-script <file> --input-latency\n"
                   "  Rendering: --gpu-triangles | --cpu-triangles --validate-graphics --verify-textures\n"
                   "  Profiling: --profile-graphics --sample-threads <file> --trace-services --strict-performance | --native-thread-cycles | --native-pmu\n"
                   "  Capture: --capture-graphics <dir> --capture-graphics-when-heavy --capture-display <dir> --capture-packets <file> --capture-vectors <dir> --capture-vu <dir> --vu-capture <file> <jobs> <start-ms> --capture-stop <dir>\n"
                   "  Without the game: --replay-packets <file> --vu-regions check|cpu\n"
                   "See docs/commands.md.\n";
#ifdef ANYPS2_NATIVE_IOP
        std::cerr<<"Linked IOP investigation: --probe-iop-reset <module.data> (stops without acknowledging reset)\n";
        std::cerr<<"Explicit selected-bundle execution: --iop-reset-session <module.data> --disc <image.iso>\n";
        std::cerr<<"Explicit default environment: --iop-default-session <module.data> (linked compiled startup modules)\n";
#endif
        return 2;
    }
#ifdef ANYPS2_NATIVE_ISA
    if (!nt::host_supports_isa(ANYPS2_NATIVE_ISA)) {
        std::cerr<<"Converted code requires an " ANYPS2_NATIVE_ISA " CPU; this host lacks required instructions. "
                   "Reconfigure with -DANYPS2_NATIVE_ISA=x86-64 and rebuild.\n";
        return 2;
    }
#endif
    try {
        std::uint64_t budget=1000000;
        unsigned run_seconds=0,disc_media=0;
        std::filesystem::path disc,memory_card,capture_directory,stop_directory,graphics_directory,vector_directory,sample_path;
        bool profile_graphics=false,validate_graphics=false,verify_textures=false,gpu_triangles=true,gpu_triangles_required=false,capture_display_only=false,capture_graphics_when_heavy=false;
        unsigned resolution_scale=5; // --resolution-scale 1 draws only the console samples
        bool start_fullscreen=false;
        bool has_budget=false,native_cycles=false,native_pmu=false,strict_performance=false,show_window=true,hold_window=true;
#ifdef ANYPS2_NATIVE_IOP
        std::filesystem::path iop_data;
        bool iop_session=false,iop_default=false;
#endif
        std::optional<nt::input::PadScript> pad_script;
        std::string_view vu_region_mode;
        bool measure_input_latency=false;
#ifdef ANYPS2_NATIVE_VU1
        VuJobCapture vu_job_capture;
#endif
        std::filesystem::path packet_capture_path,packet_replay_path;
        for (int index=2;index<argc;++index) {
            const std::string_view text=argv[index];
            if (text=="--native-thread-cycles") { native_cycles=true; continue; }
            if (text=="--native-pmu") { native_pmu=true; continue; }
            if (text=="--strict-performance") { strict_performance=true; continue; }
            if (text=="--display-stop") { show_window=hold_window=true; continue; }
            if (text=="--close-on-stop") { show_window=true; hold_window=false; continue; }
            if (text=="--headless") { show_window=false; continue; }
            if (text=="--validate-graphics") { validate_graphics=true; continue; }
            if (text=="--verify-textures") { verify_textures=true; continue; }
            if (text=="--gpu-triangles") { gpu_triangles=gpu_triangles_required=true; continue; }
            if (text=="--cpu-triangles") { gpu_triangles=false; continue; }
            if (text=="--profile-graphics") { profile_graphics=true; continue; }
            if (text=="--trace-services") { trace_services=true; continue; }
            if (text=="--pad-script") {
                if (pad_script || index+1==argc) throw std::runtime_error("Expected one pad script path");
                const auto data=nt::read_file(argv[++index],1U<<20);
                pad_script.emplace();
                const auto error=pad_script->parse(std::string_view(reinterpret_cast<const char*>(data.data()),data.size()));
                if (!error.empty()) throw std::runtime_error(error);
                continue;
            }
            if (text=="--input-latency") { measure_input_latency=true; continue; }
#ifdef ANYPS2_NATIVE_VU1
            if (text=="--vu-capture") {
                if (vu_job_capture.file.is_open() || index+3>=argc) throw std::runtime_error("Expected --vu-capture <new-file> <jobs> <start-ms>");
                const std::filesystem::path path=argv[++index];
                if (std::filesystem::exists(path)) throw std::runtime_error("VU capture output already exists");
                vu_job_capture.remaining=std::stoull(argv[++index]);
                vu_job_capture.begin=std::chrono::steady_clock::now()+std::chrono::milliseconds(std::stoull(argv[++index]));
                vu_job_capture.file.open(path,std::ios::binary);
                if (!vu_job_capture.file) throw std::runtime_error("Cannot create VU capture output");
                const std::uint32_t header[2]{0x31554a56U,static_cast<std::uint32_t>(sizeof(nt::vu::State))}; // "VJU1", record size
                vu_job_capture.file.write(reinterpret_cast<const char*>(header),sizeof(header));
                continue;
            }
#endif
            if (text=="--vu-regions") {
                if (!vu_region_mode.empty() || index+1==argc) throw std::runtime_error("Expected one VU region mode");
                vu_region_mode=argv[++index];
                if (vu_region_mode!="check" && vu_region_mode!="cpu") throw std::runtime_error("VU region mode must be check or cpu");
#ifndef ANYPS2_NATIVE_VU1_REGIONS
                throw std::runtime_error("--vu-regions requires a build linked with converted VU1 regions");
#endif
                continue;
            }
            if (text=="--run-seconds") {
                if (run_seconds || index+1==argc) throw std::runtime_error("Expected one diagnostic duration");
                const std::string_view value=argv[++index];
                const auto parsed=std::from_chars(value.data(),value.data()+value.size(),run_seconds);
                if (parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size() || !run_seconds || run_seconds>86400)
                    throw std::runtime_error("Diagnostic duration must be 1..86400 seconds");
                continue;
            }
#ifdef ANYPS2_NATIVE_IOP
            if (text=="--probe-iop-reset" || text=="--iop-reset-session" || text=="--iop-default-session") {
                if (!iop_data.empty() || index+1==argc) throw std::runtime_error("Expected one linked IOP data path");
                iop_default=text=="--iop-default-session";
                iop_session=text!="--probe-iop-reset";
                iop_data=argv[++index]; continue;
            }
#endif
            if (text=="--capture-graphics" || text=="--capture-display") {
                if (!graphics_directory.empty() || index+1==argc)
                    throw std::runtime_error("Expected one private graphics capture directory");
                capture_display_only=text=="--capture-display";
                graphics_directory=argv[++index];
                if (std::filesystem::exists(graphics_directory)) throw std::runtime_error("Graphics capture directory already exists");
                continue;
            }
            if (text=="--capture-graphics-when-heavy") { capture_graphics_when_heavy=true; continue; }
            if (text=="--fullscreen") { start_fullscreen=true; continue; }
            if (text=="--resolution-scale") {
                if (index+1==argc) throw std::runtime_error("Expected a resolution scale");
                resolution_scale=static_cast<unsigned>(std::stoul(argv[++index]));
                continue;
            }
            if (text=="--sample-threads") {
                if (!sample_path.empty() || index+1==argc) throw std::runtime_error("Expected one thread sample path");
                sample_path=argv[++index];
                continue;
            }
            if (text=="--capture-stop") {
                if (!stop_directory.empty() || index+1==argc)
                    throw std::runtime_error("Expected one private stop capture directory");
                stop_directory=argv[++index];
                if (std::filesystem::exists(stop_directory))
                    throw std::runtime_error("Stop capture directory already exists");
                continue;
            }
            if (text=="--capture-packets" || text=="--replay-packets") {
                auto& path=text=="--capture-packets" ? packet_capture_path : packet_replay_path;
                if (!path.empty() || index+1==argc) throw std::runtime_error("Expected one private packet capture file");
                path=argv[++index];
#ifndef ANYPS2_NATIVE_VU1
                if (text=="--replay-packets") throw std::runtime_error("--replay-packets requires a build linked with converted VU1 code");
#endif
                continue;
            }
            if (text=="--capture-vectors") {
                if (!vector_directory.empty() || index+1==argc) throw std::runtime_error("Expected one private vector capture directory");
                vector_directory=argv[++index];
                if (std::filesystem::exists(vector_directory)) throw std::runtime_error("Vector capture directory already exists");
                continue;
            }
            if (text=="--capture-vu") {
                if (!capture_directory.empty() || index+1==argc) throw std::runtime_error("Expected one private VU capture directory");
                capture_directory=argv[++index];
                if (std::filesystem::exists(capture_directory)) throw std::runtime_error("VU capture directory already exists");
                continue;
            }
            if (text=="--disc") {
                if (!disc.empty() || index+1==argc) throw std::runtime_error("Expected one disc image path");
                disc=argv[++index]; continue;
            }
            if (text=="--memory-card") {
                if (!memory_card.empty() || index+1==argc) throw std::runtime_error("Expected one memory card folder");
                memory_card=argv[++index]; continue;
            }
            if (text=="--disc-media") {
                if (disc_media || index+1==argc) throw std::runtime_error("Expected one disc media kind: cd or dvd");
                const std::string_view kind=argv[++index];
                if (kind!="cd" && kind!="dvd") throw std::runtime_error("Disc media must be cd or dvd");
                disc_media=kind=="cd" ? 1U : 2U; continue;
            }
            if (has_budget) throw std::runtime_error("Unknown probe option");
            const auto parsed=std::from_chars(text.data(),text.data()+text.size(),budget);
            if (parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size()) throw std::runtime_error("Invalid operation budget or option");
            has_budget=true;
        }
        if (trace_services) std::cout<<std::unitbuf;
        auto ram=nt::read_file(argv[1],0x2000000);
        if (ram.size()!=ANYPS2_EE_DATA_SIZE || nt::fingerprint(ram)!=ANYPS2_EE_DATA_HASH)
            throw std::runtime_error("Converted program data mismatch");
        ram.resize(0x2000000,0);
        nt::ee::Memory memory{ram.data(),ram.size()};
        active_program_memory=&memory;
        const auto owner_thread=std::this_thread::get_id();
        nt::ee::NativePerformanceCounters cycles([owner_thread]() -> std::optional<std::uint64_t> {
            if (std::this_thread::get_id()!=owner_thread) return std::nullopt;
#ifdef _WIN32
            ULONG64 value=0;
            if (QueryThreadCycleTime(GetCurrentThread(),&value)) return value;
#endif
            return std::nullopt;
        });
        if (unsigned(native_cycles)+unsigned(native_pmu)+unsigned(strict_performance)>1)
            throw std::runtime_error("Select only one native performance provider");
        const bool native_clock=!native_cycles && !native_pmu && !strict_performance;
        std::uint64_t clock_frequency=0;
        if (native_clock) {
#ifdef _WIN32
            LARGE_INTEGER frequency{};
            if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart>0)
                clock_frequency=static_cast<std::uint64_t>(frequency.QuadPart);
#endif
            if (!clock_frequency) throw std::runtime_error("Native performance clock is unavailable");
        }
        nt::ee::NativePerformanceCounters clock([]() -> std::optional<std::uint64_t> {
#ifdef _WIN32
            LARGE_INTEGER value{};
            if (QueryPerformanceCounter(&value) && value.QuadPart>=0)
                return static_cast<std::uint64_t>(value.QuadPart);
#endif
            return std::nullopt;
        },nt::ee::NativePerformanceCounters::ClockPolicy::elapsed_time,clock_frequency);
        if (native_clock) {
            clock.attach(memory);
            std::cout<<"metrics=native-qpc units=elapsed-clock-ticks frequency="<<clock_frequency
                     <<" count-frequency=294912000 cycle-and-cache-profiling=elapsed-time includes=waits-and-native-services\n";
        }
        std::unique_ptr<nt::host::LiveCounters> live_counters;
        std::unique_ptr<nt::ee::NativePerformanceCounters> pmu;
        if (native_pmu) {
            live_counters=std::make_unique<nt::host::LiveCounters>();
            pmu=std::make_unique<nt::ee::NativePerformanceCounters>([&] {
                if (trace_services) std::cerr<<"native_pmu_sample_begin\n";
                const auto sample=live_counters->sample();
                if (trace_services) std::cerr<<"native_pmu_sample_end valid="<<sample.has_value()<<'\n';
                return sample;
            },true);
            pmu->attach(memory);
            std::cout<<"metrics=native-pmu units=host-cpu-events includes=native-services sampling=completed-intervals\n";
        }
        if (native_cycles) {
            cycles.attach(memory);
            std::cout<<"metrics=native-thread-cycles units=host-cpu-cycles includes=native-services cache-events=unsupported\n";
        }
        std::unique_ptr<probe::NativeWindow> window;
        NativeGpu gpu;
        gpu.profile_commands=profile_graphics;
        if (profile_graphics) gpu.drawing.profile=&gpu.drawing_profile;
        gpu.verify_textures=verify_textures;
        gpu.gpu_triangles=gpu_triangles; gpu.gpu_triangles_required=gpu_triangles_required;
        if (!graphics_directory.empty()) gpu.capture=std::make_unique<probe::GraphicsCapture>(graphics_directory,capture_display_only);
        if (capture_graphics_when_heavy && !gpu.capture) throw std::runtime_error("--capture-graphics-when-heavy requires --capture-graphics");
        if (capture_graphics_when_heavy) gpu.capture->hold();
#ifdef _WIN32
        // Arms in a heavy scene, as a capture does, and writes its samples at exit.
        std::unique_ptr<probe::ThreadSampler> thread_sampler;
        if (!sample_path.empty()) thread_sampler=std::make_unique<probe::ThreadSampler>(sample_path);
#endif
        nt::ee::NativePlatform platform; platform.attach(memory);
        platform.scratchpad_copy.protection=native_code_overlap;
        platform.video_output.protection=native_code_overlap;
        platform.packet_submission.protection=native_code_overlap;
        platform.debug_byte=[](void*,std::uint8_t byte) {
            std::cerr.put(static_cast<char>(byte));
            return std::cerr ? nt::ee::Stop::budget : nt::ee::Stop::unsupported_system;
        };
        // Direct GS packets join the ordered render queue without waiting for it
        // to be recorded; parse errors stop the program when the queue reports them.
        platform.packet_setup.graphics={&gpu,NativeGpu::submit,NativeGpu::fence,true};
        VuCapture capture;
#if defined(ANYPS2_NATIVE_VU) || defined(ANYPS2_NATIVE_VU1)
        nt::ee::NativeVuUploads compiled_uploads;
        nt::ee::NativeVuSubmissions compiled_submissions;
        nt::ee::NativeVuLaunch::Profile vector_profile{},previous_vector_profile{};
#endif
#ifdef ANYPS2_NATIVE_VU
        auto compiled_vu=nt::vu::linked_binding();
        compiled_uploads.add(compiled_vu);
        nt::ee::NativeVuLaunch compiled_vu_launch(compiled_vu,100000,&platform.vector_owner.storage);
        if (profile_graphics) compiled_vu_launch.profile=&vector_profile;
        compiled_submissions.add(compiled_vu_launch);
        compiled_vu_launch.connect_macro(memory);
        compiled_vu_launch.connect_data(memory);
#endif
#ifdef ANYPS2_NATIVE_VU1
        auto compiled_vu1=nt::vu::linked_binding1();
#ifdef ANYPS2_NATIVE_VU1_REGIONS
        VuRegions vu_regions;
        if (!vu_region_mode.empty()) {
            if (compiled_vu1.artifacts()!=1) throw std::runtime_error("Linked VU1 regions describe a single-image artifact");
            compiled_vu1.substitute(0,vu_regions.substitutes(vu_region_mode=="cpu"));
        }
#endif
        compiled_uploads.add(compiled_vu1);
        nt::ee::NativeVuLaunch compiled_vu1_launch(compiled_vu1);
        compiled_vu1_launch.enable_async();
        if (profile_graphics) compiled_vu1_launch.profile=&vector_profile;
        compiled_submissions.add(compiled_vu1_launch);
        compiled_vu1_launch.connect_data(memory);
        compiled_vu1_launch.graphics={&gpu,NativeGpu::submit};
        compiled_vu1_launch.graphics.take=NativeGpu::take;
        if (vu_job_capture.file.is_open()) compiled_vu1_launch.launch_observer={&vu_job_capture,VuJobCapture::observe};
#endif
#if defined(ANYPS2_NATIVE_VU) || defined(ANYPS2_NATIVE_VU1)
        compiled_uploads.connect(platform.packet_setup);
        compiled_submissions.connect(platform.packet_setup);
#endif
#ifdef ANYPS2_NATIVE_VU1
        if (!packet_replay_path.empty()) {
            probe::PacketReplaySink replay_sink;
            platform.packet_setup.graphics={&replay_sink,probe::PacketReplaySink::submit,probe::PacketReplaySink::fence,true};
            compiled_vu1_launch.graphics={&replay_sink,probe::PacketReplaySink::submit};
            compiled_vu1_launch.graphics.take=probe::PacketReplaySink::take;
#ifdef _WIN32
            if (thread_sampler) thread_sampler->arm();
#endif
            const auto replayed=probe::replay_packets(packet_replay_path,platform,memory,replay_sink,std::cout);
            if (profile_graphics) std::cout<<"packet_replay_vector jobs="<<vector_profile.jobs<<" job_ms="<<vector_profile.job_ns/1e6
                <<" compiled_ms="<<vector_profile.compiled_ns/1e6<<" overhead_ms="<<(vector_profile.job_ns-vector_profile.compiled_ns)/1e6<<'\n';
            return replayed;
        }
#endif
        if (!capture_directory.empty()) {
            platform.packet_setup.upload_context=&capture;
            platform.packet_setup.uncompiled_upload=VuCapture::observe;
            platform.packet_setup.uncompiled_packet=VuCapture::observe_packet;
        }
        gpu.completion=&platform.graphics;
        platform.graphics.context=&gpu; platform.graphics.synchronize=NativeGpu::synchronize; platform.graphics.poll_drawing=NativeGpu::poll;
        platform.graphics.readback_status=[](void* context) -> nt::ee::IoRead {
            auto& gpu=*static_cast<NativeGpu*>(context);
            try { gpu.join(); return {nt::ee::Stop::budget,static_cast<std::uint32_t>(gpu.drawing.readback_bytes())}; }
            catch (const std::exception& error) { gpu.error=error.what(); return {nt::ee::Stop::unsupported_system}; }
        };
        platform.graphics.readback=[](void* context,std::span<std::uint8_t> output) {
            auto& gpu=*static_cast<NativeGpu*>(context);
            try { gpu.join(); gpu.drawing.readback(output); return nt::ee::Stop::budget; }
            catch (const std::exception& error) { gpu.error=error.what(); return nt::ee::Stop::unsupported_system; }
        };
        platform.graphics.reset_drawing=[](void* context) {
            auto& gpu=*static_cast<NativeGpu*>(context);
            if (gpu.capture) gpu.capture->reset();
            gpu.drawing.reset(); return nt::ee::Stop::budget;
        };
        nt::ee::NativeSifCommandPeer command_peer(platform.sif_link,memory,native_code_overlap,0x1000,&platform.sif_receive);
        nt::ee::NativeBootstrapModules bootstrap_modules;
        nt::ee::NativeTransferBuffers upload_buffers;
        auto sample_output=nt::audio::make_output();
        nt::ee::NativeAudioRpc sample_audio(upload_buffers,*sample_output);
        if (!command_peer.register_service(upload_buffers) || !command_peer.register_service(sample_audio))
            throw std::runtime_error("Cannot register native sample audio services");
        command_peer.data_upload([&](std::span<const nt::ee::NativeSifPacket> packets) { return upload_buffers.upload(packets); });
        nt::input::Gamepads gamepads;
        // A pad script replaces the host controllers; its clock starts with execution.
        std::chrono::steady_clock::time_point script_epoch=std::chrono::steady_clock::now();
        const auto sample_pad=[&](unsigned port) {
            if (!pad_script) return gamepads.sample(port);
            if (port) return nt::input::PadSnapshot{};
            const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-script_epoch);
            // A scripted pad is a standard controller whose motors produce no output.
            auto snapshot=pad_script->sample(static_cast<std::uint64_t>(elapsed.count()));
            snapshot.rumble=true;
            return snapshot;
        };
        const auto rumble_pad=[&](unsigned port,std::uint16_t low,std::uint16_t high) {
            return pad_script ? port==0 : gamepads.rumble(port,low,high);
        };
        nt::ee::NativePadRpc pads([&](unsigned port) { return sample_pad(port); },
            rumble_pad);
        nt::ee::NativePadRpc::Auxiliary pad_auxiliary;
        if (!command_peer.register_service(pads) || !command_peer.register_service(pad_auxiliary))
            throw std::runtime_error("Cannot register native controller services");
        for (unsigned port=0;port<2;++port)
            if (!pad_script && gamepads.sample(port).connected) std::cout<<"native_controller port="<<port<<" name="<<gamepads.name(port)<<'\n';
        command_peer.data_reader([&](std::uint32_t source,std::span<std::uint8_t> output) { return bootstrap_modules.snapshot(source,output); });
        if (!command_peer.register_service(bootstrap_modules)) throw std::runtime_error("Cannot register bootstrap module service");
#ifdef ANYPS2_NATIVE_IOP
        profile_iop=profile_graphics;
        std::uint64_t previous_iop_time=0;
        std::unique_ptr<probe::IopReset> iop_reset;
        if (!iop_data.empty()) {
            iop_reset=std::make_unique<probe::IopReset>(iop_data,disc,memory,platform,native_code_overlap,iop_session,iop_default,disc_media,
                [&](unsigned port) { return sample_pad(port); },
                rumble_pad);
            // Saves belong to the user, so by default they live outside any game's output.
            if (memory_card.empty()) if (const auto* local=std::getenv("LOCALAPPDATA")) memory_card=std::filesystem::path(local)/"AnyPS2"/"memory-cards";
            iop_reset->set_memory_card(memory_card);
            if (!command_peer.reset_owner(iop_reset->owner())) throw std::runtime_error("Cannot bind compiled IOP reset probe");
            active_iop=iop_reset.get();
        }
#endif
        auto files=std::make_unique<nt::ee::NativeFileRpc>(disc,[](std::uint32_t fd,nt::ByteView data) {
            auto* stream=fd==1 ? stdout : stderr;
            const auto count=std::fwrite(data.data(),1,data.size(),stream);
            if (std::fflush(stream)!=0) return std::int32_t{-5};
            return static_cast<std::int32_t>(count);
        });
        auto cdvd=std::make_unique<nt::ee::NativeCdvdInitRpc>(disc,0x4000,0x5000,disc_media);
        nt::ee::NativeCdvdScmdRpc scmd(*cdvd,0x6000,0x7000);
        // A program may use the disc before rebooting the IOP, so the initial
        // environment offers the same native disc services as a reset session.
        nt::ee::NativeCdvdSearchRpc search(*cdvd,0x20000,0x21000);
        nt::ee::NativeCdvdReadRpc read(*cdvd,0x22000,0x23000);
        nt::ee::NativeCdvdReadyRpc ready(*cdvd,0x8000059AU,0x24000,0x25000);
        nt::ee::NativeCdvdReadyRpc ready_alternate(*cdvd,0x8000059CU,0x26000,0x27000);
        if (!command_peer.register_service(*cdvd) || !command_peer.register_service(scmd) ||
            !command_peer.register_service(search) || !command_peer.register_service(read) ||
            !command_peer.register_service(ready) || !command_peer.register_service(ready_alternate))
            throw std::runtime_error("Cannot register native CD/DVD and clock services");
        if (!command_peer.register_service(*files)) throw std::runtime_error("Cannot register native file service");
        if (!command_peer.connect(platform.sif_outbound)) throw std::runtime_error("Cannot connect native command peer");
        auto state=nt::ee::NativePlatform::initial_state(ANYPS2_EE_ENTRY);
        nt::ee::StartupServices services(native_code_overlap,nt_ee_generated_contains,ANYPS2_EE_SYSCALL_TABLE);
        nt::ee::ExecutionProfile execution_profile{},previous_execution{};
        services.poll_native_jobs=[&] { return platform.poll_submissions(); };
        services.native_jobs_pending=[&] { return platform.packet_setup.working(); };
        services.program_waiting=[&] { platform.packet_setup.release_delayed_interrupts(); };
#ifdef ANYPS2_NATIVE_VU1
        services.native_jobs_completed=[&] { return compiled_vu1_launch.completed_jobs(); };
#endif
        if (profile_graphics) services.profile=&execution_profile;
        nt::ee::NativeGraphics::Profile display_profile{},previous_display{};
        // Vector capture delimits frames by displayed-buffer changes.
        if (profile_graphics || !vector_directory.empty()) platform.graphics.profile=&display_profile;
        std::optional<probe::PacketCapture> packet_capture;
        if (!packet_capture_path.empty()) {
            packet_capture.emplace(packet_capture_path);
            platform.packet_setup.observe_context=&*packet_capture;
            platform.packet_setup.observe_submission=probe::PacketCapture::observe_submission;
            platform.packet_setup.observe_packet=probe::PacketCapture::observe_packet;
        }
        std::optional<probe::VectorCapture> vector_capture;
        if (!vector_directory.empty()) {
            vector_capture.emplace(vector_directory,std::span<const std::uint8_t>(ram.data(),ram.size()),&display_profile.buffer_changes);
            platform.packet_setup.observe_context=&*vector_capture;
            platform.packet_setup.observe_submission=probe::VectorCapture::observe;
        }
        probe::ServiceCapture service_observation;
        if (!stop_directory.empty()) service_capture=&service_observation;
        active_services=&services;
        if (service_capture) {
            platform.interrupts.observation_context=&platform;
            platform.interrupts.observe_delivery=[](void* context,bool dma,unsigned line,const nt::runtime::State& state) {
                auto& platform=*static_cast<nt::ee::NativePlatform*>(context);
                const auto control=dma && line<3 ? platform.packet_submission.read(0x10008000+line*0x1000).value : 0;
                service_capture->deliver({dma,line,state.pc,control});
            };
        }
        services.publish_image=[](nt::ee::Memory& m,std::uint32_t entry) { return nt_ee_generated_publish_image(&m,entry); };
#ifdef ANYPS2_NATIVE_IOP
        if (iop_reset) services.idle_pump=[&] { return iop_reset->pump(); };
        services.console_output=[](std::string_view text) { std::cerr<<"program_console: "<<text; };
#endif
        if (show_window) {
            window=std::make_unique<probe::NativeWindow>(validate_graphics,resolution_scale,start_fullscreen);
            gpu.renderer=&window->renderer();
            gpu.renderer->verify_texture_kernels(verify_textures);
            gpu.renderer->profile_commands(profile_graphics);
            if (profile_graphics) std::cout<<"graphics_device="<<gpu.renderer->device_name()<<" validation="<<validate_graphics
                <<" ordered_fragment_access="<<gpu.renderer->statistics().ordered_fragment_access
                <<" llvm_texture_kernels="<<gpu.renderer->statistics().llvm_texture_kernels<<'\n';
        }
        const auto started=std::chrono::steady_clock::now();
        script_epoch=started;
        if (measure_input_latency && !pad_script) throw std::runtime_error("--input-latency requires --pad-script");
        std::optional<probe::InputLatency> latency;
        if (measure_input_latency) latency.emplace(*pad_script);
        if (pad_script) std::cout<<"pad_script presses="<<pad_script->presses().size()<<" end_ms="<<pad_script->end_ms()<<'\n';
        auto next_profile=started+std::chrono::seconds(5);
        nt::graphics::RenderStatistics previous_statistics{};
        nt::graphics::NativeDrawing::Profile previous_drawing{};
        auto previous_render_time=gpu.render_time;
        auto previous_drain_time=gpu.drain_time;
        // Written by presentation tasks on the render thread; read and reset by
        // this thread only after joining it.
        struct PresentTiming {
            double seconds=0;
            std::vector<double> intervals;
            std::chrono::steady_clock::time_point last_update{};
            unsigned updates=0;
        };
        const auto presented=std::make_shared<PresentTiming>();
        double previous_presentation=0;
        auto profile_started=started;
        auto next_window_poll=started;
        auto next_input=started;
        auto last_input=started;
        double input_gap_max_ms=0,input_gap_total_ms=0;
        double input_interval_gap_max_ms=0;
        std::uint64_t input_polls=0;
        nt::ee::Stop input_stop=nt::ee::Stop::budget;
        std::array<std::uint16_t,2> last_buttons{};
        std::uint64_t input_button_changes=0;
        bool timed_out=false,window_closed=false,application_exit=false,display_failed=false;
        const auto display_field=[&] { return platform.graphics.scanout_odd() ?
            nt::graphics::DisplayField::odd : nt::graphics::DisplayField::even; };
        auto present_output=[&] {
            // Headless runs still present to a display capture.
            if (gpu.capture) gpu.capture->mark_update();
            const bool capturing=gpu.capture && gpu.capture->ready();
            const auto script_ms=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now()-script_epoch).count());
            const bool timing_input=latency && latency->wants(script_ms);
            if ((!window && !capturing && !timing_input) || window_closed || display_failed) return;
            try {
                const auto& output=platform.graphics.output();
                auto plan=nt::graphics::plan_display(output.control,output.buffers,output.displays,
                    platform.graphics.half_height_output(),std::uint32_t(output.background),display_field(),platform.graphics.clocks_per_pixel());
                if (!plan) return;
                if (window) window->fit(*plan);
                if (window && window->presentable()) {
                    gpu.ensure_renderer();
                    if (window->needs_resize(*plan)) { gpu.join(); window->prepare(*plan); }
                    // Shown after this frame's queued drawing, on the render thread.
                    gpu.worker->enqueue([display=window.get(),timing=presented,plan=*plan,profile=profile_graphics,gpu=&gpu](nt::graphics::VulkanRenderer& renderer) mutable {
                        const auto present_started=std::chrono::steady_clock::now();
                        plan.smoothing=gpu->jitter_hold.smoothing(gpu->drawing.take_offset_jitter(),plan.half_height!=0);
                        const bool updated=display->present_display(renderer,plan);
                        const auto completed=std::chrono::steady_clock::now();
                        if (profile && updated) {
                            if (timing->last_update!=std::chrono::steady_clock::time_point{})
                                timing->intervals.push_back(std::chrono::duration<double,std::milli>(completed-timing->last_update).count());
                            timing->last_update=completed; ++timing->updates;
                        }
                        timing->seconds+=std::chrono::duration<double>(completed-present_started).count();
                    });
                    window->update_rate();
                }
                if (timing_input) {
                    gpu.ensure_renderer(); gpu.join();
                    latency->observe(script_ms,gpu.renderer->scanout_readback(*plan));
                }
                if (capturing) {
                    gpu.join();
                    const auto frame=gpu.renderer->scanout_readback(*plan);
                    gpu.capture->present(frame,gpu.drawing.submitted_draws(),output.control,output.buffers,output.displays,platform.graphics.half_height_output(),platform.graphics.scanout_odd());
                }
            } catch (const std::exception& error) { display_failed=true; gpu.error=error.what(); }
        };
        platform.graphics.scanout_context=&present_output;
        platform.graphics.scanout_ready=[](void* raw) { (*static_cast<decltype(present_output)*>(raw))(); };
        services.stop_requested=[&] {
            if (NativeGpu::poll(&gpu)!=nt::ee::Stop::budget) return true;
            const auto now=std::chrono::steady_clock::now();
            if (profile_graphics && now>=next_profile && gpu.renderer) {
                gpu.join();
                const auto& s=gpu.renderer->statistics(); const auto& p=previous_statistics;
                if (capture_graphics_when_heavy && s.render_passes-p.render_passes>50000) gpu.capture->arm();
#ifdef _WIN32
                if (thread_sampler && s.render_passes-p.render_passes>20000) thread_sampler->arm();
                if (packet_capture && s.render_passes-p.render_passes>20000) packet_capture->arm();
#endif
                auto& update_intervals=presented->intervals;
                std::sort(update_intervals.begin(),update_intervals.end());
                const auto percentile=[&](double fraction) {
                    return update_intervals.empty() ? 0.0 : update_intervals[static_cast<std::size_t>((update_intervals.size()-1)*fraction)];
                };
                std::vector<std::pair<std::uint32_t,nt::ee::ExecutionProfile::Site>> sites(
                    execution_profile.sites.begin(),execution_profile.sites.end());
                std::sort(sites.begin(),sites.end(),[](const auto& a,const auto& b) {
                    return a.second.nanoseconds>b.second.nanoseconds;
                });
                for (std::size_t i=0;i<std::min<std::size_t>(8,sites.size());++i) {
                    const auto& [pc,site]=sites[i];
                    std::cout<<"compiled_slice_profile pc=0x"<<std::hex<<pc<<std::dec
                             <<" ms="<<site.nanoseconds/1e6<<" operations="<<site.operations<<" calls="<<site.calls<<'\n';
                }
                execution_profile.sites.clear();
                std::vector<std::pair<std::uint64_t,nt::ee::ExecutionProfile::Site>> waits(
                    execution_profile.job_wait_sites.begin(),execution_profile.job_wait_sites.end());
                std::sort(waits.begin(),waits.end(),[](const auto& a,const auto& b) {
                    return a.second.nanoseconds>b.second.nanoseconds;
                });
                for (std::size_t i=0;i<std::min<std::size_t>(6,waits.size());++i) {
                    const auto& [key,site]=waits[i];
                    std::cout<<"job_wait_profile pc=0x"<<std::hex<<(key>>8)<<std::dec<<" stop="<<(key&255)
                             <<" waits="<<site.calls<<" sleep_ms="<<site.nanoseconds/1e6<<'\n';
                }
                execution_profile.job_wait_sites.clear();
                for (const auto& [key,wake]:execution_profile.idle_wakes)
                    if (wake.nanoseconds>=10000000)
                        std::cout<<"idle_wake_profile intc=0x"<<std::hex<<(key&0xffff)<<" dmac=0x"<<(key>>16)<<std::dec
                                 <<" wakes="<<wake.calls<<" idle_ms="<<wake.nanoseconds/1e6<<'\n';
                execution_profile.idle_wakes.clear();
#if defined(ANYPS2_NATIVE_VU) || defined(ANYPS2_NATIVE_VU1)
                std::cout<<"vector_profile compiled_ms="<<(vector_profile.compiled_ns-previous_vector_profile.compiled_ns)/1e6
                         <<" blocks="<<(vector_profile.blocks-previous_vector_profile.blocks)
                         <<" jobs="<<(vector_profile.jobs-previous_vector_profile.jobs)
                         <<" job_ms="<<(vector_profile.job_ns-previous_vector_profile.job_ns)/1e6
                         <<" submit_ms="<<(vector_profile.submit_ns-previous_vector_profile.submit_ns)/1e6
                         <<" longest_job_ms="<<vector_profile.longest_job_ns/1e6
                         <<" longest_submit_ms="<<vector_profile.longest_submit_ns/1e6<<'\n';
                std::vector<std::pair<unsigned,nt::vu::ExecutionProfile::Site>> vector_sites;
                for (unsigned i=0;i<vector_profile.execution.sites.size();++i) {
                    const auto& site=vector_profile.execution.sites[i];
                    if (site.samples) vector_sites.emplace_back(i,site);
                }
                const auto estimated=[](const auto& site) { return double(site.nanoseconds)/site.samples*site.calls; };
                std::sort(vector_sites.begin(),vector_sites.end(),[&](const auto& a,const auto& b) {
                    return estimated(a.second)>estimated(b.second);
                });
                for (unsigned i=0;i<std::min<std::size_t>(8,vector_sites.size());++i) {
                    const auto& [index,site]=vector_sites[i];
                    std::cout<<"vector_block_profile unit="<<index/2048<<" entry="<<index%2048<<" calls="<<site.calls
                        <<" entries="<<site.entries<<" samples="<<site.samples<<" sampled_ns_per_call="<<double(site.nanoseconds)/site.samples
                        <<" estimated_ms="<<estimated(site)/1e6<<'\n';
                }
                vector_profile.execution.sites.clear();
                previous_vector_profile=vector_profile;
#endif
#ifdef ANYPS2_NATIVE_IOP
                std::cout<<"iop_profile native_ms="<<(iop_time_ns-previous_iop_time)/1e6<<'\n';
                previous_iop_time=iop_time_ns;
#endif
                std::cout<<"graphics_profile seconds="<<std::chrono::duration<double>(now-started).count()
                    <<" updates_per_second="<<presented->updates/std::chrono::duration<double>(now-profile_started).count()
                    <<" update_p50_ms="<<percentile(0.50)<<" update_p95_ms="<<percentile(0.95)
                    <<" render_ms="<<std::chrono::duration<double,std::milli>(gpu.render_time-previous_render_time).count()
                    <<" command_parse_ms="<<(gpu.drawing_profile.parse_ns-previous_drawing.parse_ns)/1e6
                    <<" command_validate_ms="<<(gpu.drawing_profile.validate_ns-previous_drawing.validate_ns)/1e6
                    <<" command_submit_ms="<<(gpu.drawing_profile.submit_ns-previous_drawing.submit_ns)/1e6
                    <<" gpu_triangle_batches="<<gpu.drawing.gpu_packet_batches()
                    <<" present_ms="<<(presented->seconds-previous_presentation)*1000
                    <<" command_drain_ms="<<std::chrono::duration<double,std::milli>(gpu.drain_time-previous_drain_time).count()
                    <<" input_poll_gap_max_ms="<<input_interval_gap_max_ms
                    <<" compiled_entry_ms="<<(execution_profile.entry_ns-previous_execution.entry_ns)/1e6
                    <<" longest_compiled_entry_ms="<<execution_profile.longest_entry_ns/1e6
                    <<" syscall_ms="<<(execution_profile.service_ns-previous_execution.service_ns)/1e6
                    <<" idle_sleep_ms="<<(execution_profile.idle_ns-previous_execution.idle_ns)/1e6
                    <<" display_waits="<<(execution_profile.display_waits-previous_execution.display_waits)
                    <<" native_job_waits="<<(execution_profile.job_waits-previous_execution.job_waits)
                    <<" display_sleep_ms="<<(execution_profile.display_sleep_ns-previous_execution.display_sleep_ns)/1e6
                    <<" native_job_sleep_ms="<<(execution_profile.job_sleep_ns-previous_execution.job_sleep_ns)/1e6
                    <<" compiled_entries="<<execution_profile.entries-previous_execution.entries
                    <<" retired_operations="<<execution_profile.operations-previous_execution.operations
                    <<" status_reads="<<display_profile.status_reads-previous_display.status_reads
                    <<" finish_not_ready_reads="<<display_profile.finish_not_ready-previous_display.finish_not_ready
                    <<" vsync_not_ready_reads="<<display_profile.vsync_not_ready-previous_display.vsync_not_ready
                    <<" display_buffer_changes="<<display_profile.buffer_changes-previous_display.buffer_changes
                    <<" circuit_changes="<<display_profile.circuit_changes[0]-previous_display.circuit_changes[0]<<','<<display_profile.circuit_changes[1]-previous_display.circuit_changes[1]
                    <<" vsync_clear_to_observe_ms="<<(display_profile.vsync_clear_to_observe_ns-previous_display.vsync_clear_to_observe_ns)/1e6
                    <<" vsync_observations="<<display_profile.vsync_observations-previous_display.vsync_observations
                    <<" vsync_deadlines="<<display_profile.vsync_deadlines-previous_display.vsync_deadlines
                    <<" vsync_coalesced="<<display_profile.vsync_coalesced-previous_display.vsync_coalesced
                    <<" vsync_acknowledged="<<display_profile.vsync_acknowledged-previous_display.vsync_acknowledged
                    <<" scanout_missed="<<display_profile.scanout_missed-previous_display.scanout_missed
                    <<" draw_ms="<<(s.draw_ns-p.draw_ns)/1e6<<" write_ms="<<(s.write_ns-p.write_ns)/1e6
                    <<" gpu_timing_supported="<<s.gpu_timing_supported<<" gpu_ms="<<(s.gpu_ns-p.gpu_ns)/1e6<<" vector_gpu_ms="<<(s.vector_gpu_ns-p.vector_gpu_ns)/1e6
                    <<" wait_ms="<<(s.wait_ns-p.wait_ns)/1e6<<" upload_ms="<<(s.upload_ns-p.upload_ns)/1e6
                    <<" surface_lookup_ms="<<(s.surface_lookup_ns-p.surface_lookup_ns)/1e6
                    <<" texture_misses="<<(s.texture_misses-p.texture_misses)<<" texture_evictions="<<(s.texture_evictions-p.texture_evictions)
                    <<" texture_lookup_ms="<<(s.texture_lookup_ns-p.texture_lookup_ns)/1e6
                    <<" target_bind_ms="<<(s.target_bind_ns-p.target_bind_ns)/1e6
                    <<" draw_record_ms="<<(s.draw_record_ns-p.draw_record_ns)/1e6
                    <<" invalidate_ms="<<(s.invalidate_ns-p.invalidate_ns)/1e6
                    <<" decode_ms="<<(s.texture_decode_ns-p.texture_decode_ns)/1e6
                    <<" surface_import_ms="<<(s.surface_import_ns-p.surface_import_ns)/1e6
                    <<" surface_export_ms="<<(s.surface_export_ns-p.surface_export_ns)/1e6
                    <<" surface_conflicts="<<(s.surface_conflicts-p.surface_conflicts)
                    <<" surface_gpu_imports="<<(s.surface_gpu_imports-p.surface_gpu_imports)
                    <<" surface_evictions="<<(s.surface_evictions-p.surface_evictions)
                    <<" texture_readbacks="<<(s.texture_readbacks-p.texture_readbacks)
                    <<" palette_readbacks="<<(s.palette_readbacks-p.palette_readbacks)
                    <<" conflict_view="<<s.last_surface_conflict[0]<<','<<s.last_surface_conflict[1]<<','<<s.last_surface_conflict[2]<<','<<s.last_surface_conflict[3]
                    <<" conflict_other="<<s.last_surface_conflict[4]<<','<<s.last_surface_conflict[5]<<','<<s.last_surface_conflict[6]<<','<<s.last_surface_conflict[7]
                    <<" texture_kernels="<<(s.texture_kernel_dispatches-p.texture_kernel_dispatches)
                    <<" texture_kernels_verified="<<(s.texture_kernel_verified-p.texture_kernel_verified)
                    <<" validation_errors="<<gpu.renderer->validation_errors()
                    <<" surface_palette_loads="<<(s.surface_palette_loads-p.surface_palette_loads)
                    <<" memory_reads="<<(s.memory_reads-p.memory_reads)<<" memory_read_words="<<(s.memory_read_words-p.memory_read_words)
                    <<" memory_read_ms="<<(s.read_ns-p.read_ns)/1e6
                    <<" texture_recycles="<<(s.texture_recycles-p.texture_recycles)
                    <<" submissions="<<s.submissions-p.submissions<<" uploads="<<s.uploads-p.uploads
                    <<" triangles="<<s.triangles-p.triangles<<" barriers="<<s.feedback_barriers-p.feedback_barriers
                    <<" draw_calls="<<s.draw_calls-p.draw_calls<<" batched_primitives="<<s.batched_primitives-p.batched_primitives<<" render_passes="<<s.render_passes-p.render_passes<<" gpu_palette_loads="<<s.gpu_palette_loads-p.gpu_palette_loads<<" gpu_indexed_decodes="<<s.gpu_indexed_decodes-p.gpu_indexed_decodes<<" shader_indexed_draws="<<s.shader_indexed_draws-p.shader_indexed_draws<<" ordering_barriers="<<s.ordering_barriers-p.ordering_barriers<<'\n'<<std::flush;
                if (gpu.gpu_triangles) for (const auto& [reason,count]:gpu.drawing.gpu_packet_fallbacks())
                    std::cout<<"gpu_triangle_fallback count="<<count<<" reason="<<reason<<'\n';
                presented->intervals.clear(); presented->updates=0; profile_started=now;
                input_interval_gap_max_ms=0;
                previous_drain_time=gpu.drain_time;
                previous_statistics=s; previous_render_time=gpu.render_time; previous_presentation=presented->seconds;
                previous_drawing=gpu.drawing_profile;
                previous_execution=execution_profile;
                previous_display=display_profile;
                next_profile=now+std::chrono::seconds(5);
            }
            timed_out=run_seconds && now-started>=std::chrono::seconds(run_seconds);
            if (timed_out) return true;
            if (now>=next_input) {
                const auto gap=std::chrono::duration<double,std::milli>(now-last_input).count();
                input_gap_max_ms=std::max(input_gap_max_ms,gap);
                input_interval_gap_max_ms=std::max(input_interval_gap_max_ms,gap);
                input_gap_total_ms+=gap; ++input_polls; last_input=now;
                next_input=now+std::chrono::milliseconds(4);
                gamepads.update();
                input_stop=pads.update(memory,native_code_overlap);
                if (input_stop!=nt::ee::Stop::budget) return true;
                for (unsigned port=0;port<2;++port) {
                    const auto buttons=sample_pad(port).buttons;
                    if (buttons!=last_buttons[port]) { ++input_button_changes; last_buttons[port]=buttons; }
                }
            }
            if (window && !window_closed && now>=next_window_poll) {
                next_window_poll=now+std::chrono::milliseconds(16);
                if (!window->poll()) {
                    window_closed=true;
                }
            }
#ifdef ANYPS2_NATIVE_IOP
            if (iop_reset) {
                // Drain the real close callback even when the EE is no longer
                // making IOP calls. A diagnostic boundary still stops normally.
                if (window_closed) iop_reset->request_close();
                if (window_closed && !iop_reset->exit_requested() && iop_reset->pump()!=nt::ee::Stop::budget) return true;
                application_exit=iop_reset->exit_requested();
                return application_exit;
            }
#endif
            return window_closed;
        };
        auto stop=nt::ee::run(execute,state,memory,budget,services);
        try { gpu.join(); } catch (const std::exception& error) { gpu.error=error.what(); }
        if (input_stop!=nt::ee::Stop::budget) stop=input_stop;
        if (!gpu.error.empty()) stop=nt::ee::Stop::unsupported_system;
        std::cout<<"native_input_polls="<<input_polls<<" mean_poll_gap_ms="
                 <<(input_polls ? input_gap_total_ms/input_polls : 0)<<" max_poll_gap_ms="<<input_gap_max_ms<<'\n';
        nt::audio::StreamState sample_state;
        const auto sample_state_ok=sample_audio.playback_state(sample_state);
        std::cout<<"native_sample_uploads="<<sample_audio.uploads()<<" plays="<<sample_audio.plays()
                 <<" consumed_frames="<<sample_state.frames<<" queued="<<sample_state.queued
                 <<" state_valid="<<sample_state_ok<<" last_method="<<sample_audio.last_method()
                 <<" error="<<sample_output->error()<<'\n';
        std::cout<<"native_controller_button_changes="<<input_button_changes<<'\n';
        if (latency) latency->report(std::cout);
#ifdef ANYPS2_NATIVE_VU1_REGIONS
        if (!vu_region_mode.empty())
            std::cout<<"vu_regions mode="<<vu_region_mode<<" entries="<<vu_regions.entries<<" guard_accepted="<<vu_regions.accepted
                     <<" guard_rejected="<<vu_regions.rejected<<" executed="<<vu_regions.executed<<" declined="<<vu_regions.failed
                     <<" settled="<<vu_regions.settled[0]<<","<<vu_regions.settled[1]<<","<<vu_regions.settled[2]<<","<<vu_regions.settled[3]<<","<<vu_regions.settled[4]<<'\n';
        if (!vu_region_mode.empty()) for (const auto& [reason,count]:vu_regions.reasons)
            std::cout<<"vu_region_rejection count="<<count<<" reason="<<reason<<'\n';
        if (!vu_region_mode.empty()) for (const auto& [key,count]:vu_regions.transitions)
            if (count>=1000) std::cout<<"vu_region_chain from="<<std::get<0>(key)<<" to="<<std::get<1>(key)
                                       <<" same_buffer="<<std::get<2>(key)<<" count="<<count<<'\n';
        if (!vu_region_mode.empty()) {
            std::cout<<"vu_observed_loads="<<observed_loads<<'\n';
            for (const auto& [head,executions]:region_row_executions)
                std::cout<<"vu_region_outputs head="<<head<<" executions="<<executions
                         <<" read_by_vu_code="<<region_row_reads[head]<<'\n';
        }
#endif
        const auto execution_seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
        std::cout<<"execution_seconds="<<execution_seconds
                 <<" render_seconds="<<std::chrono::duration<double>(gpu.render_time).count();
        if (gpu.renderer) {
            const auto& statistics=gpu.renderer->statistics();
            std::cout<<" gpu_submissions="<<statistics.submissions<<" gpu_sprites="<<statistics.sprites<<" gpu_triangles="<<statistics.triangles
                     <<" gpu_upload_bytes="<<statistics.upload_bytes<<" gpu_readback_bytes="<<statistics.readback_bytes
                     <<" gpu_texture_snapshot_bytes="<<statistics.texture_snapshot_bytes
                     <<" gpu_texture_kernels="<<statistics.texture_kernel_dispatches
                     <<" gpu_texture_kernels_verified="<<statistics.texture_kernel_verified
                     <<" gpu_validation_errors="<<gpu.renderer->validation_errors()
                     <<" gpu_direct_texture_draws="<<statistics.direct_texture_draws;
        }
        std::cout<<'\n';
        if (timed_out || window_closed || application_exit)
            std::cout<<"host_stop="<<(timed_out ? "diagnostic-deadline" : application_exit ? "application-exit" : "window-closed")<<'\n';
        if (const auto thread=services.current_thread())
            std::cout<<"ee_thread="<<thread->id<<" status="<<thread->status<<" wait_semaphore="<<thread->semaphore<<'\n';
        for (const auto& thread:services.thread_snapshot(state))
            std::cout<<"ee_thread_state="<<thread.id<<" status="<<thread.status<<" semaphore="<<thread.semaphore
                     <<" priority="<<thread.priority<<" wakeups="<<thread.wakeups<<" pc="<<nt::hex32(thread.pc)
                     <<" return="<<nt::hex32(thread.return_address)<<" stack="<<nt::hex32(thread.stack)<<'\n';
#ifdef ANYPS2_NATIVE_IOP
        if (iop_reset && iop_reset->invoked()) {
            const auto result=iop_reset->result();
            std::cout<<"iop_reset_accepted="<<iop_reset->accepted()<<" entries_returned="<<iop_reset->returned()
                     <<" startup_finished="<<iop_reset->finished()<<" executed="<<iop_reset->executed()
                     <<" reason="<<static_cast<unsigned>(result.reason)<<" pc="<<nt::hex32(result.pc)
                     <<" address="<<nt::hex32(result.address)<<" trap="<<nt::hex32(result.trap_code)
                     <<" generations="<<iop_reset->generations()<<" last_bind="<<nt::hex32(iop_reset->last_server())
                     <<" interrupts_enabled="<<iop_reset->interrupts_enabled()
                     <<" ee_receiver_control="<<nt::hex32(platform.sif_receive.read(0x1000c000).value)
                     <<" session_cdvd_initialized="<<iop_reset->cdvd_initialized()
                     <<" native_pad_polls="<<iop_reset->pads().polls()
                     <<" native_pad_rejected="<<nt::hex32(iop_reset->pads().rejected_command())
                     <<" native_pad_argument="<<iop_reset->pads().rejected_argument()
                     <<" native_pad_config="<<iop_reset->pads().rejected_config()
                     <<" native_pad_rumble="<<iop_reset->pads().rejected_rumble()
                     <<" serial_failure="<<iop_reset->serial().failure()
                     <<" serial_sent="<<iop_reset->serial().pending_output().size()
                     <<" serial_expected="<<iop_reset->serial().expected_output()
                     <<" serial_receive="<<iop_reset->serial().expected_input()
                     <<" cdvd_command="<<iop_reset->cdvd_command()
                     <<" cdvd_media="<<iop_reset->cdvd_media()
                     <<" cdvd_argument="<<iop_reset->cdvd_argument()
                     <<" cdvd_ready_calls="<<iop_reset->cdvd_ready_calls()
                     <<" cdvd_ready_mode="<<iop_reset->cdvd_ready_mode()
                     <<" cdvd_ready_result="<<iop_reset->cdvd_ready_result()
                     <<" boot_device_queries="<<iop_reset->boot_device_queries()
                     <<" boot_device="<<iop_reset->boot_device()
                     <<" boot_device_decision="<<iop_reset->boot_device_decision()
                     <<" boot_device_path="<<iop_reset->boot_device_path()
                     <<" load_decision="<<iop_reset->load_decision()
                     <<" load_bytes="<<iop_reset->load_bytes()
                     <<" load_hash="<<iop_reset->load_hash()
                     <<" load_path="<<iop_reset->load_path()
                     <<" native_audio_ready="<<iop_reset->audio().ready()
                     <<" native_audio_voices="<<iop_reset->audio().voices()
                     <<" native_usb_devices="<<iop_reset->usb().device_count()
                     <<" native_usb_drivers="<<iop_reset->usb().driver_count()
                     <<" native_usb_error="<<iop_reset->usb().error()
                     <<" native_dev9_unavailable="<<iop_reset->dev9().unavailable_initializations()
                     <<" memory_card_error=\""<<iop_reset->memory_card().error()<<'"'
                     <<" native_sample_error="<<static_cast<unsigned>(iop_reset->sound().sample_error())
                     <<" native_sample_voice="<<iop_reset->sound().rejected_voice()
                     <<" native_sample_transfer="<<iop_reset->sound().transfer_failure()
                     <<" native_audio_error="<<iop_reset->audio().error()<<'\n';
            if (iop_reset->sound().sample_error()!=nt::audio::SampleError::none) {
                const auto sample=iop_reset->sound().rejected_sample();
                std::cout<<"native_sample_start="<<nt::hex32(sample[0])
                         <<" repeat="<<nt::hex32(sample[1])<<" pitch="<<sample[2]<<'\n';
            }
            std::cout<<"native_sound_call=";
            for (auto value:iop_reset->sound().last_call()) std::cout<<nt::hex32(value)<<' ';
            std::cout<<'\n';
            std::cout<<"native_sample_transfer_overlap=";
            for (auto value:iop_reset->sound().transfer_overlap()) std::cout<<nt::hex32(value)<<' ';
            std::cout<<'\n';
            std::cout<<"native_sample_transfer_arguments=";
            for (auto argument:iop_reset->sound().transfer_arguments()) std::cout<<nt::hex32(argument)<<' ';
            std::cout<<'\n';
            for (const auto& thread:iop_reset->thread_snapshot())
                std::cout<<"iop_thread="<<thread.id<<" status="<<thread.status<<" pc="<<nt::hex32(thread.pc)
                         <<" return="<<nt::hex32(thread.return_address)<<" stack="<<nt::hex32(thread.stack_pointer)
                         <<" priority="<<thread.priority<<" wait_kind="<<thread.wait_kind<<" wait_object="<<nt::hex32(thread.wait_object)
                         <<" wakeups="<<thread.wakeups<<" executed="<<thread.executed<<'\n';
            if (const auto packet=iop_reset->pending_iop_packet();!packet.empty()) {
                std::cout<<"iop_pending_packet_bytes="<<packet.size();
                for (std::uint32_t at=0;at+4<=packet.size();at+=4)
                    std::cout<<" word"<<at/4<<'='<<nt::hex32(nt::iop::load_native_word(packet,at));
                std::cout<<'\n';
            }
            if (const auto& transfer=iop_reset->rejected_transfer())
                std::cout<<"sif_rejected_descriptor="<<transfer->index
                         <<" source="<<nt::hex32(transfer->source)<<" destination="<<nt::hex32(transfer->destination)
                         <<" bytes="<<transfer->size<<" attributes="<<nt::hex32(transfer->attributes)<<'\n';
        }
#endif
        if (!capture_directory.empty()) capture.save(capture_directory);
        if (vector_capture) vector_capture->save();
        if (!stop_directory.empty()) {
            probe::capture_stop(stop_directory,state,memory,stop,nt_ee_generated_input_identity());
            service_observation.save(stop_directory);
#if defined(ANYPS2_NATIVE_VU) || defined(ANYPS2_NATIVE_VU1)
            const auto capture_vector_failure=[&](const nt::ee::NativeVuLaunch& owner) {
                if (!owner.failed_state) return;
                const auto& failed=*owner.failed_state;
                const auto name="vu"+std::to_string(owner.unit())+"-failure";
                std::ofstream output(stop_directory/(name+".txt"));
                output.exceptions(std::ios::failbit|std::ios::badbit);
                output<<"format=anyps2-vu-failure-v1\nentry="<<owner.last_entry<<"\nnext="<<failed.next_pair<<'\n';
                for (unsigned i=0;i<failed.integers.size();++i) output<<"vi"<<i+1<<'='<<failed.integers[i]<<'\n';
                for (unsigned i=0;i<failed.vectors.size();++i) {
                    output<<"vf"<<i+1<<'=';
                    for (auto lane:failed.vectors[i]) output<<nt::hex32(lane)<<' ';
                    output<<'\n';
                }
                output<<"clip="<<failed.clip<<"\nvisible_clip="<<failed.visible_clip
                    <<"\nstatus="<<failed.status<<"\nvisible_status="<<failed.visible_status
                    <<"\nq="<<nt::hex32(failed.quotient)<<"\nq_pending="<<failed.quotient_pending
                    <<"\nq_distance="<<failed.quotient_read_delay
                    <<"\np="<<nt::hex32(failed.scalar_p)<<"\np_pending="<<failed.p_pending
                    <<"\np_distance="<<failed.p_read_delay<<'\n';
                nt::write_file(stop_directory/(name+"-data.bin"),
                    {reinterpret_cast<const std::uint8_t*>(failed.data.data()),sizeof(failed.data)});
            };
#ifdef ANYPS2_NATIVE_VU
            capture_vector_failure(compiled_vu_launch);
#endif
#ifdef ANYPS2_NATIVE_VU1
            capture_vector_failure(compiled_vu1_launch);
#endif
#endif
#ifdef ANYPS2_NATIVE_IOP
            if (iop_reset && iop_reset->invoked()) {
                probe::capture_iop(stop_directory,iop_reset->data(),iop_reset->active_state());
                nt::write_file(stop_directory/"sound-samples.bin",iop_reset->sound().sample_storage());
            }
#endif
            if (gpu.drawing.submitted_draws()) {
                const auto& pixels=gpu.drawing.pixels();
                nt::write_file(stop_directory/"graphics-storage.bin",
                    {reinterpret_cast<const std::uint8_t*>(pixels.data()),pixels.size()*sizeof(pixels[0])});
                const auto& draw=*gpu.drawing.last_draw();
                probe::capture_image(stop_directory/"last-render-target.ppm",draw.right,draw.bottom,
                    [&](unsigned x,unsigned y) { return pixels[nt::graphics::rgba_word(draw.base,draw.pitch,x,y)]; });
                const auto& output=platform.graphics.output();
                const auto plan=display_failed ? std::nullopt : nt::graphics::plan_display(output.control,output.buffers,output.displays,
                    platform.graphics.half_height_output(),std::uint32_t(output.background),display_field());
                const auto frame=plan ? std::optional(gpu.renderer->scanout_readback(*plan)) : std::nullopt;
                if (frame) probe::capture_image(stop_directory/"selected-display.ppm",frame->width,frame->height,
                    [&](unsigned x,unsigned y) { return frame->pixels[y*frame->width+x]; });
            }
        }
        if (display_failed) {
            const auto& output=platform.graphics.output();
            std::cout<<"display_failure_control="<<std::hex<<output.control
                     <<" buffer0="<<output.buffers[0]<<" buffer1="<<output.buffers[1]
                     <<" display0="<<output.displays[0]<<" display1="<<output.displays[1]<<std::dec<<'\n';
        }
        std::cout<<"graphics_draws="<<gpu.drawing.submitted_draws()<<" half_height_output="<<platform.graphics.half_height_output()<<'\n';
        std::cout<<"native_completion_status="<<nt::hex32(platform.completions.status())
                 <<" native_interrupt_pending="<<nt::hex32(platform.interrupts.read(0x1000f000).value)
                 <<" native_interrupt_enabled="<<nt::hex32(platform.interrupts.read(0x1000f010).value)<<'\n';
        for (unsigned channel=0;channel<3;++channel) {
            std::cout<<"packet_channel="<<channel<<" control="<<nt::hex32(platform.packet_submission.read(0x10008000+channel*0x1000).value)
                     <<" consumer_pending="<<platform.packet_setup.pending(channel);
            if (channel<2) std::cout<<" vif_cycle="<<nt::hex32(platform.packet_setup.read(0x10003840+channel*0x400).value)
                <<" vif_mode="<<platform.packet_setup.read(0x10003850+channel*0x400).value;
            std::cout<<'\n';
        }
        std::cout<<"stop="<<static_cast<unsigned>(stop)<<" pc="<<nt::hex32(state.pc)
                 <<" executed="<<state.executed<<" v0="<<state.gpr[2].lo
                 <<(stop==nt::ee::Stop::syscall ? " syscall=" : " v1=")<<state.gpr[3].lo;
        if (stop==nt::ee::Stop::memory_fault || stop==nt::ee::Stop::code_write || stop==nt::ee::Stop::unpublished_code || (stop==nt::ee::Stop::unsupported_system && memory.fault_address))
            std::cout<<" address="<<nt::hex32(memory.fault_address);
        if (stop==nt::ee::Stop::unmapped_code) std::cout<<" return_address="<<nt::hex32(static_cast<std::uint32_t>(state.gpr[31].lo));
        if (stop==nt::ee::Stop::unsupported_system && memory.exception.unavailable_selector)
            std::cout<<" unavailable_exception_selector="<<memory.exception.unavailable_selector;
        std::cout<<" native_image="<<memory.executable_image;
        if (memory.metrics.rejected) {
            const auto control=memory.metrics.rejected_control;
            std::cout<<" unsupported_performance_control="<<nt::hex32(control)
                     <<" event0="<<((control>>5)&31)<<" event1="<<((control>>15)&31)
                     <<" metric_provider="<<(memory.metrics.request ? "rejected" : "missing")
                     <<" metric_operation="<<static_cast<unsigned>(memory.metrics.rejected_operation)
                     <<" metric_counter="<<static_cast<unsigned>(memory.metrics.rejected_counter);
        }
        if (native_clock) std::cout<<" native_metric_samples="<<clock.samples()<<" native_clock_reads="<<clock.clock_reads()
            <<" native_metric_error="<<static_cast<unsigned>(clock.error());
        if (native_cycles) std::cout<<" native_metric_samples="<<cycles.samples()
            <<" native_metric_error="<<static_cast<unsigned>(cycles.error());
        if (pmu) std::cout<<" native_metric_samples="<<pmu->samples()
            <<" native_metric_error="<<static_cast<unsigned>(pmu->error())
            <<" native_pmu_error="<<live_counters->error();
        if (pmu) if (const auto measured=pmu->last_sample())
            std::cout<<" measured_native_cycles="<<(*measured)[0]<<" measured_native_icache_misses="<<(*measured)[1];
        if (stop==nt::ee::Stop::unmapped_code && state.gpr[3].lo==7)
            std::cout<<" uncompiled_image_entry="<<nt::hex32(memory.fault_address);
        if (stop==nt::ee::Stop::unmapped_code && state.gpr[3].lo==0x74)
            std::cout<<" requested_syscall="<<state.gpr[4].lo
                     <<" uncompiled_handler="<<nt::hex32(static_cast<std::uint32_t>(state.gpr[5].lo));
        if (stop==nt::ee::Stop::unmapped_code && (state.gpr[3].lo==0x20 || state.gpr[3].lo==0x22))
            std::cout<<" uncompiled_thread_entry="<<nt::hex32(memory.fault_address);
        if (stop!=nt::ee::Stop::budget && command_peer.last_command())
            std::cout<<" native_peer_command="<<nt::hex32(command_peer.last_command())<<" option="<<command_peer.last_option();
        if (stop!=nt::ee::Stop::budget) for (const auto& packet:platform.sif_outbound.pending_packets()) {
            std::cout<<" sif_destination="<<nt::hex32(packet.destination)<<" sif_bytes="<<packet.bytes.size();
            if (packet.bytes.size()>=16) std::cout<<" sif_header="<<nt::hex32(nt::le32(packet.bytes,0))
                <<" sif_command="<<nt::hex32(nt::le32(packet.bytes,8));
        }
        if (cdvd && !cdvd->error().empty()) std::cerr<<"disc initialization: "<<cdvd->error()<<'\n';
        if (!gpu.error.empty()) std::cerr<<"graphics: "<<gpu.error<<'\n';
        if (!platform.packet_setup.rejected_packet().empty()) std::cerr<<"graphics packet: "<<platform.packet_setup.rejected_packet()<<'\n';
        std::cout<<" video_decoded_blocks="<<platform.video.decoded_blocks()<<" video_converted_blocks="<<platform.video.converted_blocks();
        if ((platform.video.read(0x10002010).value&0x80000000U) ||
            (memory.fault_address>=0x10002000 && memory.fault_address<0x10002040) ||
            (memory.fault_address>=0x1000b000 && memory.fault_address<=0x1000b480)) {
            std::cout<<" video_control="<<nt::hex32(platform.video.read(0x10002010).value)
                <<" video_position="<<nt::hex32(platform.video.read(0x10002020).value)
                <<" video_output_control="<<nt::hex32(platform.video_output.read(0x1000b000).value)
                <<" video_output_remaining="<<platform.video_output.read(0x1000b020).value
                <<" video_resets="<<platform.video.resets()
                <<" video_input_pending="<<platform.video.submission_pending()
                <<" video_submission_control="<<nt::hex32(platform.video_submission.read(0x10008000).value)
                <<" video_submission_source="<<nt::hex32(platform.video_submission.read(0x10008010).value)
                <<" video_submission_qwords="<<platform.video_submission.read(0x10008020).value
                <<" video_submission_chain="<<nt::hex32(platform.video_submission.read(0x10008030).value)
                <<" video_submission_source_status="<<unsigned(platform.video_submission.read(0x10008010).status)
                <<" submissions_enabled="<<platform.packet_submission.policy.enabled();
        }
        if (nt::ee::NativePacketSubmission::maps(memory.fault_address)) {
            const auto base=memory.fault_address&~0xfffU;
            const auto channel=(base-0x10008000)/0x1000;
            std::cout<<" packet_channel="<<channel
                     <<" packet_source="<<nt::hex32(platform.packet_submission.read(base+0x10).value)
                     <<" packet_qwords="<<platform.packet_submission.read(base+0x20).value
                     <<" packet_chain="<<nt::hex32(platform.packet_submission.read(base+0x30).value)
                     <<" submissions_enabled="<<platform.packet_submission.policy.enabled()
                     <<" packet_pending="<<platform.packet_setup.pending(channel)
                     <<" rejected_vif_command="<<platform.packet_setup.rejected_command();
        }
        if (trace_services || stop==nt::ee::Stop::unsupported_vector || stop==nt::ee::Stop::unsupported_numeric) {
#ifdef ANYPS2_NATIVE_VU
            std::cout<<" compiled_vu_unit="<<nt_vu_program_unit()<<" compiled_vu_ready="<<compiled_vu.ready()
                <<" vu0_run_status="<<static_cast<unsigned>(compiled_vu_launch.last_result.status)
                <<" vu0_blocks="<<compiled_vu_launch.last_result.blocks
                <<" vu0_entry="<<compiled_vu_launch.last_entry<<" vu0_next_pair="<<compiled_vu_launch.last_next_pair;
#endif
#ifdef ANYPS2_NATIVE_VU1
            std::cout<<" compiled_vu1_ready="<<compiled_vu1.ready()
                <<" vu1_run_status="<<static_cast<unsigned>(compiled_vu1_launch.last_result.status)
                <<" vu1_blocks="<<compiled_vu1_launch.last_result.blocks
                <<" vu1_sequence_launches="<<compiled_vu1_launch.last_sequence.launches
                <<" vu1_sequence_blocks="<<compiled_vu1_launch.last_sequence.blocks
                <<" vu1_largest_launch="<<compiled_vu1_launch.last_sequence.largest_launch
                <<" vu1_entry="<<compiled_vu1_launch.last_entry<<" vu1_next_pair="<<compiled_vu1_launch.last_next_pair;
#endif
            if (gpu.renderer) std::cout<<" graphics_resets="<<platform.graphics.resets()<<" gpu=\""<<gpu.renderer->device_name()<<"\"";
            if (cdvd) std::cout<<" cdvd_initialized="<<cdvd->initialized();
            if (files) {
                const auto stats=files->statistics();
                std::cout<<" file_requests="<<stats.requests<<" file_opens="<<stats.opens<<" file_bytes="<<stats.bytes_read<<" file_method="<<stats.last_method;
            }
            if (const auto server=command_peer.last_unavailable_server())
                std::cout<<" unavailable_rpc_server="<<nt::hex32(*server);
            std::cout<<" status="<<nt::hex32(state.cop0_status)<<" pending="<<nt::hex32(memory.pending_interrupts)
                     <<" a0="<<nt::hex32(static_cast<std::uint32_t>(state.gpr[4].lo))
                     <<" a1="<<nt::hex32(static_cast<std::uint32_t>(state.gpr[5].lo));
        }
        std::cout<<'\n';
        if (window && !application_exit && !window_closed && !display_failed) {
            const auto& output=platform.graphics.output();
            const auto frame=nt::graphics::scanout(gpu.drawing.pixels(),output.control,output.buffers,output.displays,platform.graphics.half_height_output(),std::uint32_t(output.background),display_field());
            if (frame) {
                std::cout<<"native_display="<<frame->width<<'x'<<frame->height<<" execution=stopped\n";
            }
            std::cout<<std::flush;
            if (hold_window) window->hold(frame);
        }
        return application_exit ? 0 : stop==nt::ee::Stop::budget ? 4 : 3;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
