#include "capture.hpp"
#include <nt/ee_services.hpp>
#include <nt/ee_platform.hpp>
#include <nt/ee_sif_command_peer.hpp>
#include <nt/binary.hpp>
#include <nt/ee_file_rpc.hpp>
#include <nt/ee_cdvd_rpc.hpp>
#include <nt/ee_performance.hpp>
#include <nt/live_counters.hpp>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <memory>
#include "display.hpp"
#include <nt/vulkan_renderer.hpp>
#include <nt/native_drawing.hpp>
#include <charconv>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <thread>
#ifdef ANYPS2_NATIVE_IOP
#include "iop_reset.hpp"
#endif
#if defined(ANYPS2_NATIVE_VU) || defined(ANYPS2_NATIVE_VU1)
#include <nt/vu_artifact.hpp>
#include <nt/ee_vu_upload.hpp>
#include <nt/ee_vu_launch.hpp>
#endif
extern "C" nt::ee::Stop nt_ee_generated_run(nt::runtime::State*,nt::ee::Memory*,std::uint64_t);
extern "C" bool nt_ee_generated_active_code_overlap(std::uint32_t,std::uint32_t,std::uint32_t);
extern "C" nt::ee::Stop nt_ee_generated_publish_image(nt::ee::Memory*,std::uint32_t);
extern "C" bool nt_ee_generated_contains(std::uint32_t);
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
    std::unique_ptr<nt::graphics::VulkanRenderer> renderer;
    nt::graphics::NativeDrawing drawing;
    std::string error;
    static bool submit(void* raw,std::span<const nt::graphics::NativeDrawing::Packet> packets) {
        auto& self=*static_cast<NativeGpu*>(raw);
        if (synchronize(raw)!=nt::ee::Stop::budget) return false;
        try { self.drawing.submit(*self.renderer,packets); return true; }
        catch (const std::exception& error) { self.error=error.what(); return false; }
    }
    static nt::ee::Stop synchronize(void* context) {
        auto& self=*static_cast<NativeGpu*>(context);
        try {
            if (!self.renderer) self.renderer=std::make_unique<nt::graphics::VulkanRenderer>();
            self.renderer->synchronize();
            return nt::ee::Stop::budget;
        } catch (const std::exception& error) {
            self.error=error.what(); return nt::ee::Stop::unsupported_system;
        }
    }
};
bool trace_services=false;
#ifdef ANYPS2_NATIVE_IOP
probe::IopReset* active_iop=nullptr;
#endif
nt::ee::Stop execute(nt::runtime::State* state,nt::ee::Memory* memory,std::uint64_t budget) {
#ifdef ANYPS2_NATIVE_IOP
    if (active_iop) {
        const auto result=active_iop->pump();
        if (result!=nt::ee::Stop::budget) return result;
    }
#endif
    const auto stop=nt_ee_generated_run(state,memory,budget);
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
        std::cerr<<"Usage: ps2-native <program.data> [operation-budget] [--trace-services] [--run-seconds N] [--headless | --display-stop] [--strict-performance | --native-thread-cycles | --native-pmu] [--disc <image.iso>] [--capture-vu <new-private-directory>] [--capture-stop <new-private-directory>]\n";
#ifdef ANYPS2_NATIVE_IOP
        std::cerr<<"Linked IOP investigation: --probe-iop-reset <module.data> (stops without acknowledging reset)\n";
        std::cerr<<"Explicit selected-bundle execution: --iop-reset-session <module.data> --disc <image.iso>\n";
#endif
        return 2;
    }
    try {
        std::uint64_t budget=1000000;
        unsigned run_seconds=0;
        std::filesystem::path disc,capture_directory,stop_directory;
        bool has_budget=false,native_cycles=false,native_pmu=false,strict_performance=false,display_stopped=true;
#ifdef ANYPS2_NATIVE_IOP
        std::filesystem::path iop_data;
        bool iop_session=false;
#endif
        for (int index=2;index<argc;++index) {
            const std::string_view text=argv[index];
            if (text=="--native-thread-cycles") { native_cycles=true; continue; }
            if (text=="--native-pmu") { native_pmu=true; continue; }
            if (text=="--strict-performance") { strict_performance=true; continue; }
            if (text=="--display-stop") { display_stopped=true; continue; }
            if (text=="--headless") { display_stopped=false; continue; }
            if (text=="--trace-services") { trace_services=true; continue; }
            if (text=="--run-seconds") {
                if (run_seconds || index+1==argc) throw std::runtime_error("Expected one diagnostic duration");
                const std::string_view value=argv[++index];
                const auto parsed=std::from_chars(value.data(),value.data()+value.size(),run_seconds);
                if (parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size() || !run_seconds || run_seconds>86400)
                    throw std::runtime_error("Diagnostic duration must be 1..86400 seconds");
                continue;
            }
#ifdef ANYPS2_NATIVE_IOP
            if (text=="--probe-iop-reset" || text=="--iop-reset-session") {
                if (!iop_data.empty() || index+1==argc) throw std::runtime_error("Expected one linked IOP data path");
                iop_session=text=="--iop-reset-session";
                iop_data=argv[++index]; continue;
            }
#endif
            if (text=="--capture-stop") {
                if (!stop_directory.empty() || index+1==argc)
                    throw std::runtime_error("Expected one private stop capture directory");
                stop_directory=argv[++index];
                if (std::filesystem::exists(stop_directory))
                    throw std::runtime_error("Stop capture directory already exists");
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
        nt::ee::NativePerformanceCounters clock([]() -> std::optional<std::uint64_t> {
#ifdef _WIN32
            LARGE_INTEGER value{};
            if (QueryPerformanceCounter(&value) && value.QuadPart>=0)
                return static_cast<std::uint64_t>(value.QuadPart);
#endif
            return std::nullopt;
        },nt::ee::NativePerformanceCounters::ClockPolicy::elapsed_time);
        if (native_clock) {
#ifdef _WIN32
            LARGE_INTEGER frequency{};
            if (QueryPerformanceFrequency(&frequency) && frequency.QuadPart>0)
                clock_frequency=static_cast<std::uint64_t>(frequency.QuadPart);
#endif
            if (!clock_frequency) throw std::runtime_error("Native performance clock is unavailable");
            clock.attach(memory);
            std::cout<<"metrics=native-qpc units=elapsed-clock-ticks frequency="<<clock_frequency
                     <<" cycle-and-cache-profiling=elapsed-time includes=waits-and-native-services\n";
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
        NativeGpu gpu;
        nt::ee::NativePlatform platform; platform.attach(memory);
        platform.debug_byte=[](void*,std::uint8_t byte) {
            std::cerr.put(static_cast<char>(byte));
            return std::cerr ? nt::ee::Stop::budget : nt::ee::Stop::unsupported_system;
        };
        platform.packet_setup.graphics={&gpu,NativeGpu::submit,[](void* raw) {
            return NativeGpu::synchronize(raw)==nt::ee::Stop::budget;
        },true};
        VuCapture capture;
#if defined(ANYPS2_NATIVE_VU) || defined(ANYPS2_NATIVE_VU1)
        nt::ee::NativeVuUploads compiled_uploads;
        nt::ee::NativeVuSubmissions compiled_submissions;
#endif
#ifdef ANYPS2_NATIVE_VU
        auto compiled_vu=nt::vu::linked_binding();
        compiled_uploads.add(compiled_vu);
        nt::ee::NativeVuLaunch compiled_vu_launch(compiled_vu,100000,&platform.vector_owner.storage);
        compiled_submissions.add(compiled_vu_launch);
        compiled_vu_launch.connect_macro(memory);
        compiled_vu_launch.connect_data(memory);
#endif
#ifdef ANYPS2_NATIVE_VU1
        auto compiled_vu1=nt::vu::linked_binding1();
        compiled_uploads.add(compiled_vu1);
        nt::ee::NativeVuLaunch compiled_vu1_launch(compiled_vu1);
        compiled_submissions.add(compiled_vu1_launch);
        compiled_vu1_launch.connect_data(memory);
        compiled_vu1_launch.graphics={&gpu,NativeGpu::submit};
#endif
#if defined(ANYPS2_NATIVE_VU) || defined(ANYPS2_NATIVE_VU1)
        compiled_uploads.connect(platform.packet_setup);
        compiled_submissions.connect(platform.packet_setup);
#endif
        if (!capture_directory.empty()) {
            platform.packet_setup.upload_context=&capture;
            platform.packet_setup.uncompiled_upload=VuCapture::observe;
            platform.packet_setup.uncompiled_packet=VuCapture::observe_packet;
        }
        platform.graphics.context=&gpu; platform.graphics.synchronize=NativeGpu::synchronize;
        platform.graphics.reset_drawing=[](void* context) {
            static_cast<NativeGpu*>(context)->drawing.reset(); return nt::ee::Stop::budget;
        };
        nt::ee::NativeSifCommandPeer command_peer(platform.sif_link,memory,native_code_overlap,0x1000,&platform.sif_receive);
#ifdef ANYPS2_NATIVE_IOP
        std::unique_ptr<probe::IopReset> iop_reset;
        if (!iop_data.empty()) {
            iop_reset=std::make_unique<probe::IopReset>(iop_data,disc,memory,platform,native_code_overlap,iop_session);
            if (!command_peer.reset_owner(iop_reset->owner())) throw std::runtime_error("Cannot bind compiled IOP reset probe");
            active_iop=iop_reset.get();
        }
#endif
        std::unique_ptr<nt::ee::NativeFileRpc> files;
        std::unique_ptr<nt::ee::NativeCdvdInitRpc> cdvd;
        if (!disc.empty()) {
            files=std::make_unique<nt::ee::NativeFileRpc>(disc);
            cdvd=std::make_unique<nt::ee::NativeCdvdInitRpc>(disc);
            if (!command_peer.register_service(*cdvd)) throw std::runtime_error("Cannot register native CD/DVD initialization");
            if (!command_peer.register_service(*files)) throw std::runtime_error("Cannot register native disc service");
        }
        if (!command_peer.connect(platform.sif_outbound)) throw std::runtime_error("Cannot connect native command peer");
        auto state=nt::ee::NativePlatform::initial_state(ANYPS2_EE_ENTRY);
        nt::ee::StartupServices services(native_code_overlap,nt_ee_generated_contains,ANYPS2_EE_SYSCALL_TABLE);
        services.publish_image=[](nt::ee::Memory& m,std::uint32_t entry) { return nt_ee_generated_publish_image(&m,entry); };
#ifdef ANYPS2_NATIVE_IOP
        if (iop_reset) services.idle_pump=[&] { return iop_reset->pump(); };
#endif
        std::unique_ptr<probe::NativeWindow> window;
        if (display_stopped) window=std::make_unique<probe::NativeWindow>();
        const auto started=std::chrono::steady_clock::now();
        auto next_present=started;
        bool timed_out=false,window_closed=false;
        if (window || run_seconds) services.stop_requested=[&] {
            const auto now=std::chrono::steady_clock::now();
            timed_out=run_seconds && now-started>=std::chrono::seconds(run_seconds);
            if (timed_out) return true;
            if (window && now>=next_present) {
                next_present=now+std::chrono::milliseconds(16);
                if (!window->poll()) { window_closed=true; return true; }
                const auto& output=platform.graphics.output();
                if (const auto frame=nt::graphics::scanout(gpu.drawing.pixels(),output.control,output.buffers,output.displays))
                    window->present(*frame);
            }
            return false;
        };
        const auto stop=nt::ee::run(execute,state,memory,budget,services);
        if (timed_out || window_closed)
            std::cout<<"host_stop="<<(timed_out ? "diagnostic-deadline" : "window-closed")<<'\n';
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
                     <<" session_cdvd_initialized="<<iop_reset->cdvd_initialized()
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
                     <<" load_path="<<iop_reset->load_path()<<'\n';
            if (const auto& transfer=iop_reset->rejected_transfer())
                std::cout<<"sif_rejected_descriptor="<<transfer->index
                         <<" source="<<nt::hex32(transfer->source)<<" destination="<<nt::hex32(transfer->destination)
                         <<" bytes="<<transfer->size<<" attributes="<<nt::hex32(transfer->attributes)<<'\n';
        }
#endif
        if (!capture_directory.empty()) capture.save(capture_directory);
        if (!stop_directory.empty()) {
            probe::capture_stop(stop_directory,state,memory,stop);
            if (gpu.drawing.submitted_draws()) {
                const auto& pixels=gpu.drawing.pixels();
                nt::write_file(stop_directory/"graphics-storage.bin",
                    {reinterpret_cast<const std::uint8_t*>(pixels.data()),pixels.size()*sizeof(pixels[0])});
                const auto& draw=*gpu.drawing.last_draw();
                probe::capture_image(stop_directory/"last-render-target.ppm",draw.right,draw.bottom,
                    [&](unsigned x,unsigned y) { return pixels[nt::graphics::rgba_word(draw.base,draw.pitch,x,y)]; });
                const auto& output=platform.graphics.output();
                const auto frame=nt::graphics::scanout(pixels,output.control,output.buffers,output.displays);
                if (frame) probe::capture_image(stop_directory/"selected-display.ppm",frame->width,frame->height,
                    [&](unsigned x,unsigned y) { return frame->pixels[y*frame->width+x]; });
            }
        }
        std::cout<<"graphics_draws="<<gpu.drawing.submitted_draws()<<'\n';
        std::cout<<"stop="<<static_cast<unsigned>(stop)<<" pc="<<nt::hex32(state.pc)
                 <<" executed="<<state.executed<<" v0="<<state.gpr[2].lo<<" syscall="<<state.gpr[3].lo;
        if (stop==nt::ee::Stop::memory_fault || stop==nt::ee::Stop::code_write || stop==nt::ee::Stop::unpublished_code || (stop==nt::ee::Stop::unsupported_system && memory.fault_address))
            std::cout<<" address="<<nt::hex32(memory.fault_address);
        if (stop==nt::ee::Stop::unmapped_code) std::cout<<" return_address="<<nt::hex32(static_cast<std::uint32_t>(state.gpr[31].lo));
        std::cout<<" native_image="<<memory.executable_image;
        if (memory.metrics.rejected) {
            const auto control=memory.metrics.rejected_control;
            std::cout<<" unsupported_performance_control="<<nt::hex32(control)
                     <<" event0="<<((control>>5)&31)<<" event1="<<((control>>15)&31)
                     <<" metric_provider="<<(memory.metrics.request ? "rejected" : "missing")
                     <<" metric_operation="<<static_cast<unsigned>(memory.metrics.rejected_operation)
                     <<" metric_counter="<<static_cast<unsigned>(memory.metrics.rejected_counter);
        }
        if (native_clock) std::cout<<" native_metric_samples="<<clock.samples()
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
        if (stop==nt::ee::Stop::unsupported_system && command_peer.last_command())
            std::cout<<" native_peer_command="<<nt::hex32(command_peer.last_command())<<" option="<<command_peer.last_option();
        if (cdvd && !cdvd->error().empty()) std::cerr<<"disc initialization: "<<cdvd->error()<<'\n';
        if (!gpu.error.empty()) std::cerr<<"graphics: "<<gpu.error<<'\n';
        if (trace_services) {
#ifdef ANYPS2_NATIVE_VU
            std::cout<<" compiled_vu_unit="<<nt_vu_program_unit()<<" compiled_vu_ready="<<compiled_vu.ready();
#endif
#ifdef ANYPS2_NATIVE_VU1
            std::cout<<" compiled_vu1_ready="<<compiled_vu1.ready();
#endif
            if (nt::ee::NativePacketSubmission::maps(memory.fault_address)) {
                const auto base=memory.fault_address&~0xfffU;
                std::cout<<" packet_channel="<<((base-0x10008000)/0x1000)
                         <<" packet_source="<<nt::hex32(platform.packet_submission.read(base+0x10).value)
                         <<" packet_qwords="<<platform.packet_submission.read(base+0x20).value
                         <<" packet_chain="<<nt::hex32(platform.packet_submission.read(base+0x30).value)
                         <<" submissions_enabled="<<platform.packet_submission.policy.enabled()
                         <<" rejected_vif_command="<<platform.packet_setup.rejected_command();
            }
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
        if (window) {
            const auto& output=platform.graphics.output();
            const auto frame=nt::graphics::scanout(gpu.drawing.pixels(),output.control,output.buffers,output.displays);
            if (frame) {
                std::cout<<"native_display="<<frame->width<<'x'<<frame->height<<" execution=stopped\n";
            }
            std::cout<<std::flush;
            window->hold(frame);
        }
        return stop==nt::ee::Stop::budget ? 4 : 3;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
