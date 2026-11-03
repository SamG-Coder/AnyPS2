#include "capture.hpp"
#include "graphics_capture.hpp"
#include <nt/ee_services.hpp>
#include <nt/ee_platform.hpp>
#include <nt/ee_sif_command_peer.hpp>
#include <nt/binary.hpp>
#include <nt/ee_file_rpc.hpp>
#include <nt/ee_cdvd_rpc.hpp>
#include <nt/ee_bootstrap_modules.hpp>
#include <nt/ee_pad_rpc.hpp>
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
#include "display.hpp"
#include <nt/vulkan_renderer.hpp>
#include <nt/native_drawing.hpp>
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
#include <nt/ee_vu_upload.hpp>
#include <nt/ee_vu_launch.hpp>
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
    bool verify_textures=false;
    nt::graphics::NativeDrawing drawing{true,nt::graphics::NativeDrawing::Completion::deferred};
    void ensure_renderer() {
        if (!renderer) {
            headless_renderer=std::make_unique<nt::graphics::VulkanRenderer>();
            renderer=headless_renderer.get();
            renderer->verify_texture_kernels(verify_textures);
        }
    }
    std::unique_ptr<probe::GraphicsCapture> capture;
    nt::ee::NativeGraphics* completion=nullptr;
    std::string error;
    std::chrono::steady_clock::duration render_time{};
    static bool submit(void* raw,std::span<const nt::graphics::NativeDrawing::Packet> packets) {
        auto& self=*static_cast<NativeGpu*>(raw);
        const auto began=std::chrono::steady_clock::now();
        try {
            self.ensure_renderer();
            if (self.capture) self.capture->submit(packets);
            const auto finished=self.drawing.completed_finishes();
            self.drawing.submit(*self.renderer,packets);
            if (self.drawing.completed_finishes()!=finished && self.completion) self.completion->drawing_completed();
            self.render_time+=std::chrono::steady_clock::now()-began;
            return true;
        }
        catch (const std::exception& error) { self.error=error.what(); return false; }
    }
    static nt::ee::Stop poll(void* context) {
        auto& self=*static_cast<NativeGpu*>(context);
        if (!self.renderer) return nt::ee::Stop::budget;
        try {
            if (self.renderer->validation_errors()) {
                self.error=self.renderer->validation_message();
                return nt::ee::Stop::unsupported_system;
            }
            const auto before=self.drawing.completed_finishes();
            self.drawing.poll_completions(*self.renderer);
            if (self.drawing.completed_finishes()!=before && self.completion) self.completion->drawing_completed();
            return nt::ee::Stop::budget;
        } catch (const std::exception& error) { self.error=error.what(); return nt::ee::Stop::unsupported_system; }
    }
    static nt::ee::Stop synchronize(void* context) {
        auto& self=*static_cast<NativeGpu*>(context);
        try {
            self.ensure_renderer();
            self.renderer->synchronize();
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
#endif
nt::ee::Stop execute(nt::runtime::State* state,nt::ee::Memory* memory,std::uint64_t budget) {
#ifdef ANYPS2_NATIVE_IOP
    if (active_iop) {
        const auto result=active_iop->pump();
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
        std::cerr<<"Usage: ps2-native <program.data> [operation-budget] [--trace-services] [--profile-graphics] [--validate-graphics] [--verify-textures] [--run-seconds N] [--headless | --display-stop | --close-on-stop] [--strict-performance | --native-thread-cycles | --native-pmu] [--disc <image.iso>] [--capture-vu <new-private-directory>] [--capture-stop <new-private-directory>] [--capture-graphics <new-private-directory>]\n";
#ifdef ANYPS2_NATIVE_IOP
        std::cerr<<"Linked IOP investigation: --probe-iop-reset <module.data> (stops without acknowledging reset)\n";
        std::cerr<<"Explicit selected-bundle execution: --iop-reset-session <module.data> --disc <image.iso>\n";
#endif
        return 2;
    }
    try {
        std::uint64_t budget=1000000;
        unsigned run_seconds=0;
        std::filesystem::path disc,capture_directory,stop_directory,graphics_directory;
        bool profile_graphics=false,validate_graphics=false,verify_textures=false;
        bool has_budget=false,native_cycles=false,native_pmu=false,strict_performance=false,show_window=true,hold_window=true;
#ifdef ANYPS2_NATIVE_IOP
        std::filesystem::path iop_data;
        bool iop_session=false;
#endif
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
            if (text=="--profile-graphics") { profile_graphics=true; continue; }
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
            if (text=="--capture-graphics") {
                if (!graphics_directory.empty() || index+1==argc)
                    throw std::runtime_error("Expected one private graphics capture directory");
                graphics_directory=argv[++index];
                if (std::filesystem::exists(graphics_directory)) throw std::runtime_error("Graphics capture directory already exists");
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
        gpu.verify_textures=verify_textures;
        if (!graphics_directory.empty()) gpu.capture=std::make_unique<probe::GraphicsCapture>(graphics_directory);
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
        gpu.completion=&platform.graphics;
        platform.graphics.context=&gpu; platform.graphics.synchronize=NativeGpu::synchronize; platform.graphics.poll_drawing=NativeGpu::poll;
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
        nt::ee::NativePadRpc pads([&](unsigned port) { return gamepads.sample(port); },
            [&](unsigned port,std::uint16_t low,std::uint16_t high) { return gamepads.rumble(port,low,high); });
        nt::ee::NativePadRpc::Auxiliary pad_auxiliary;
        if (!command_peer.register_service(pads) || !command_peer.register_service(pad_auxiliary))
            throw std::runtime_error("Cannot register native controller services");
        for (unsigned port=0;port<2;++port)
            if (gamepads.sample(port).connected) std::cout<<"native_controller port="<<port<<" name="<<gamepads.name(port)<<'\n';
        command_peer.data_reader([&](std::uint32_t source,std::span<std::uint8_t> output) { return bootstrap_modules.snapshot(source,output); });
        if (!command_peer.register_service(bootstrap_modules)) throw std::runtime_error("Cannot register bootstrap module service");
#ifdef ANYPS2_NATIVE_IOP
        std::unique_ptr<probe::IopReset> iop_reset;
        if (!iop_data.empty()) {
            iop_reset=std::make_unique<probe::IopReset>(iop_data,disc,memory,platform,native_code_overlap,iop_session);
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
        auto cdvd=std::make_unique<nt::ee::NativeCdvdInitRpc>(disc);
        nt::ee::NativeCdvdScmdRpc scmd(*cdvd,0x6000,0x7000);
        if (!command_peer.register_service(*cdvd) || !command_peer.register_service(scmd))
            throw std::runtime_error("Cannot register native CD/DVD and clock services");
        if (!command_peer.register_service(*files)) throw std::runtime_error("Cannot register native file service");
        if (!command_peer.connect(platform.sif_outbound)) throw std::runtime_error("Cannot connect native command peer");
        auto state=nt::ee::NativePlatform::initial_state(ANYPS2_EE_ENTRY);
        nt::ee::StartupServices services(native_code_overlap,nt_ee_generated_contains,ANYPS2_EE_SYSCALL_TABLE);
        nt::ee::ExecutionProfile execution_profile{},previous_execution{};
        if (profile_graphics) services.profile=&execution_profile;
        nt::ee::NativeGraphics::Profile display_profile{},previous_display{};
        if (profile_graphics) platform.graphics.profile=&display_profile;
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
#endif
        std::unique_ptr<probe::NativeWindow> window;
        if (show_window) {
            window=std::make_unique<probe::NativeWindow>(validate_graphics);
            gpu.renderer=&window->renderer();
            gpu.renderer->verify_texture_kernels(verify_textures);
            if (profile_graphics) std::cout<<"graphics_device="<<gpu.renderer->device_name()<<" validation="<<validate_graphics
                <<" ordered_fragment_access="<<gpu.renderer->statistics().ordered_fragment_access
                <<" llvm_texture_kernels="<<gpu.renderer->statistics().llvm_texture_kernels<<'\n';
        }
        const auto started=std::chrono::steady_clock::now();
        auto next_profile=started+std::chrono::seconds(5);
        nt::graphics::RenderStatistics previous_statistics{};
        auto previous_render_time=gpu.render_time;
        double presentation_seconds=0,previous_presentation=0;
        std::vector<double> update_intervals;
        std::chrono::steady_clock::time_point last_update{};
        auto profile_started=started;
        unsigned profile_updates=0;
        auto next_window_poll=started;
        auto next_input=started;
        auto last_input=started;
        double input_gap_max_ms=0,input_gap_total_ms=0;
        std::uint64_t input_polls=0;
        nt::ee::Stop input_stop=nt::ee::Stop::budget;
        std::array<std::uint16_t,2> last_buttons{};
        std::uint64_t input_button_changes=0;
        bool timed_out=false,window_closed=false,application_exit=false;
        auto present_output=[&] {
            if (!window || window_closed) return;
            const auto& output=platform.graphics.output();
            if (const auto plan=nt::graphics::plan_display(output.control,output.buffers,output.displays,
                    platform.graphics.half_height_output(),std::uint32_t(output.background))) {
                const auto present_started=std::chrono::steady_clock::now();
                const auto updated=window->present(*plan);
                if (profile_graphics && updated) {
                    const auto completed=std::chrono::steady_clock::now();
                    if (last_update!=std::chrono::steady_clock::time_point{})
                        update_intervals.push_back(std::chrono::duration<double,std::milli>(completed-last_update).count());
                    last_update=completed; ++profile_updates;
                }
                presentation_seconds+=std::chrono::duration<double>(std::chrono::steady_clock::now()-present_started).count();
                if (gpu.capture) {
                    const auto frame=gpu.renderer->scanout_readback(*plan);
                    gpu.capture->present(frame,gpu.drawing.submitted_draws(),output.control,output.buffers,output.displays,platform.graphics.half_height_output());
                }
            }
        };
        platform.graphics.scanout_context=&present_output;
        platform.graphics.scanout_ready=[](void* raw) { (*static_cast<decltype(present_output)*>(raw))(); };
        services.stop_requested=[&] {
            if (NativeGpu::poll(&gpu)!=nt::ee::Stop::budget) return true;
            const auto now=std::chrono::steady_clock::now();
            if (profile_graphics && now>=next_profile && gpu.renderer) {
                const auto& s=gpu.renderer->statistics(); const auto& p=previous_statistics;
                std::sort(update_intervals.begin(),update_intervals.end());
                const auto percentile=[&](double fraction) {
                    return update_intervals.empty() ? 0.0 : update_intervals[static_cast<std::size_t>((update_intervals.size()-1)*fraction)];
                };
                std::cout<<"graphics_profile seconds="<<std::chrono::duration<double>(now-started).count()
                    <<" updates_per_second="<<profile_updates/std::chrono::duration<double>(now-profile_started).count()
                    <<" update_p50_ms="<<percentile(0.50)<<" update_p95_ms="<<percentile(0.95)
                    <<" render_ms="<<std::chrono::duration<double,std::milli>(gpu.render_time-previous_render_time).count()
                    <<" present_ms="<<(presentation_seconds-previous_presentation)*1000
                    <<" compiled_entry_ms="<<(execution_profile.entry_ns-previous_execution.entry_ns)/1e6
                    <<" syscall_ms="<<(execution_profile.service_ns-previous_execution.service_ns)/1e6
                    <<" idle_sleep_ms="<<(execution_profile.idle_ns-previous_execution.idle_ns)/1e6
                    <<" compiled_entries="<<execution_profile.entries-previous_execution.entries
                    <<" retired_operations="<<execution_profile.operations-previous_execution.operations
                    <<" status_reads="<<display_profile.status_reads-previous_display.status_reads
                    <<" finish_not_ready_reads="<<display_profile.finish_not_ready-previous_display.finish_not_ready
                    <<" vsync_not_ready_reads="<<display_profile.vsync_not_ready-previous_display.vsync_not_ready
                    <<" display_buffer_changes="<<display_profile.buffer_changes-previous_display.buffer_changes
                    <<" vsync_clear_to_observe_ms="<<(display_profile.vsync_clear_to_observe_ns-previous_display.vsync_clear_to_observe_ns)/1e6
                    <<" vsync_observations="<<display_profile.vsync_observations-previous_display.vsync_observations
                    <<" draw_ms="<<(s.draw_ns-p.draw_ns)/1e6<<" write_ms="<<(s.write_ns-p.write_ns)/1e6
                    <<" gpu_timing_supported="<<s.gpu_timing_supported<<" gpu_ms="<<(s.gpu_ns-p.gpu_ns)/1e6
                    <<" wait_ms="<<(s.wait_ns-p.wait_ns)/1e6<<" upload_ms="<<(s.upload_ns-p.upload_ns)/1e6
                    <<" decode_ms="<<(s.texture_decode_ns-p.texture_decode_ns)/1e6
                    <<" texture_kernels="<<(s.texture_kernel_dispatches-p.texture_kernel_dispatches)
                    <<" texture_kernels_verified="<<(s.texture_kernel_verified-p.texture_kernel_verified)
                    <<" validation_errors="<<gpu.renderer->validation_errors()
                    <<" submissions="<<s.submissions-p.submissions<<" uploads="<<s.uploads-p.uploads
                    <<" triangles="<<s.triangles-p.triangles<<" barriers="<<s.feedback_barriers-p.feedback_barriers
                    <<" draw_calls="<<s.draw_calls-p.draw_calls<<" batched_primitives="<<s.batched_primitives-p.batched_primitives<<'\n'<<std::flush;
                update_intervals.clear(); profile_updates=0; profile_started=now;
                previous_statistics=s; previous_render_time=gpu.render_time; previous_presentation=presentation_seconds;
                previous_execution=execution_profile;
                previous_display=display_profile;
                next_profile=now+std::chrono::seconds(5);
            }
            timed_out=run_seconds && now-started>=std::chrono::seconds(run_seconds);
            if (timed_out) return true;
            if (now>=next_input) {
                const auto gap=std::chrono::duration<double,std::milli>(now-last_input).count();
                input_gap_max_ms=std::max(input_gap_max_ms,gap);
                input_gap_total_ms+=gap; ++input_polls; last_input=now;
                next_input=now+std::chrono::milliseconds(4);
                gamepads.update();
                input_stop=pads.update(memory,native_code_overlap);
                if (input_stop!=nt::ee::Stop::budget) return true;
                for (unsigned port=0;port<2;++port) {
                    const auto buttons=gamepads.sample(port).buttons;
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
                     <<" load_path="<<iop_reset->load_path()
                     <<" native_audio_ready="<<iop_reset->audio().ready()
                     <<" native_audio_voices="<<iop_reset->audio().voices()
                     <<" native_usb_devices="<<iop_reset->usb().device_count()
                     <<" native_usb_drivers="<<iop_reset->usb().driver_count()
                     <<" native_usb_error="<<iop_reset->usb().error()
                     <<" native_dev9_unavailable="<<iop_reset->dev9().unavailable_initializations()
                     <<" native_audio_error="<<iop_reset->audio().error()<<'\n';
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
        if (!stop_directory.empty()) {
            probe::capture_stop(stop_directory,state,memory,stop,nt_ee_generated_input_identity());
            service_observation.save(stop_directory);
#ifdef ANYPS2_NATIVE_IOP
            if (iop_reset && iop_reset->invoked())
                probe::capture_iop(stop_directory,iop_reset->data(),iop_reset->active_state());
#endif
            if (gpu.drawing.submitted_draws()) {
                const auto& pixels=gpu.drawing.pixels();
                nt::write_file(stop_directory/"graphics-storage.bin",
                    {reinterpret_cast<const std::uint8_t*>(pixels.data()),pixels.size()*sizeof(pixels[0])});
                const auto& draw=*gpu.drawing.last_draw();
                probe::capture_image(stop_directory/"last-render-target.ppm",draw.right,draw.bottom,
                    [&](unsigned x,unsigned y) { return pixels[nt::graphics::rgba_word(draw.base,draw.pitch,x,y)]; });
                const auto& output=platform.graphics.output();
                const auto frame=nt::graphics::scanout(pixels,output.control,output.buffers,output.displays,platform.graphics.half_height_output());
                if (frame) probe::capture_image(stop_directory/"selected-display.ppm",frame->width,frame->height,
                    [&](unsigned x,unsigned y) { return frame->pixels[y*frame->width+x]; });
            }
        }
        std::cout<<"graphics_draws="<<gpu.drawing.submitted_draws()<<" half_height_output="<<platform.graphics.half_height_output()<<'\n';
        std::cout<<"native_completion_status="<<nt::hex32(platform.completions.status())
                 <<" native_interrupt_pending="<<nt::hex32(platform.interrupts.read(0x1000f000).value)
                 <<" native_interrupt_enabled="<<nt::hex32(platform.interrupts.read(0x1000f010).value)<<'\n';
        for (unsigned channel=0;channel<3;++channel)
            std::cout<<"packet_channel="<<channel<<" control="<<nt::hex32(platform.packet_submission.read(0x10008000+channel*0x1000).value)
                     <<" consumer_pending="<<platform.packet_setup.pending(channel)<<'\n';
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
        if (stop!=nt::ee::Stop::budget && command_peer.last_command())
            std::cout<<" native_peer_command="<<nt::hex32(command_peer.last_command())<<" option="<<command_peer.last_option();
        if (stop!=nt::ee::Stop::budget) for (const auto& packet:platform.sif_outbound.pending_packets()) {
            std::cout<<" sif_destination="<<nt::hex32(packet.destination)<<" sif_bytes="<<packet.bytes.size();
            if (packet.bytes.size()>=16) std::cout<<" sif_header="<<nt::hex32(nt::le32(packet.bytes,0))
                <<" sif_command="<<nt::hex32(nt::le32(packet.bytes,8));
        }
        if (cdvd && !cdvd->error().empty()) std::cerr<<"disc initialization: "<<cdvd->error()<<'\n';
        if (!gpu.error.empty()) std::cerr<<"graphics: "<<gpu.error<<'\n';
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
        if (trace_services) {
#ifdef ANYPS2_NATIVE_VU
            std::cout<<" compiled_vu_unit="<<nt_vu_program_unit()<<" compiled_vu_ready="<<compiled_vu.ready();
#endif
#ifdef ANYPS2_NATIVE_VU1
            std::cout<<" compiled_vu1_ready="<<compiled_vu1.ready()
                <<" vu1_run_status="<<static_cast<unsigned>(compiled_vu1_launch.last_result.status)
                <<" vu1_blocks="<<compiled_vu1_launch.last_result.blocks;
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
        if (window && !application_exit && !window_closed) {
            const auto& output=platform.graphics.output();
            const auto frame=nt::graphics::scanout(gpu.drawing.pixels(),output.control,output.buffers,output.displays,platform.graphics.half_height_output(),std::uint32_t(output.background));
            if (frame) {
                std::cout<<"native_display="<<frame->width<<'x'<<frame->height<<" execution=stopped\n";
            }
            std::cout<<std::flush;
            if (hold_window) window->hold(frame);
        }
        return application_exit ? 0 : stop==nt::ee::Stop::budget ? 4 : 3;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
