#include <nt/live_counters.hpp>
#include <nt/host_metrics.hpp>
#include <nt/etw_counters.hpp>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <objbase.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace nt::host {
struct LiveCounters::Impl {
    struct Properties { EVENT_TRACE_PROPERTIES value{}; wchar_t name[128]{}; } properties;
    DWORD owner=GetCurrentThreadId();
    CounterIntervals intervals{owner,GetActiveProcessorCount(ALL_PROCESSOR_GROUPS),2};
    TRACEHANDLE session=0,reader=INVALID_PROCESSTRACE_HANDLE;
    std::wstring name;
    EVENT_TRACE_LOGFILEW logfile{};
    std::thread pump;
    std::jthread flusher;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::string failure;
    std::uint64_t beginning=0;
    bool ended=false;
    static void checked(ULONG result,const char* action) {
        if (result) throw std::system_error(result,std::system_category(),action);
    }
    void fail(std::string message) {
        if (failure.empty()) failure=std::move(message);
        changed.notify_all();
    }
    void start() {
        std::array<ULONG,2> sources{};
        const auto available=profile_sources();
        const std::array names{L"TotalCycles",L"IcacheMisses"};
        for (unsigned i=0;i<2;++i) {
            const auto found=std::find_if(available.begin(),available.end(),[&](const auto& s) { return s.description==names[i]; });
            if (found==available.end()) throw std::runtime_error("Native cycle/cache profiling source unavailable");
            sources[i]=found->id;
        }
        auto& settings=properties.value;
        if (FAILED(CoCreateGuid(&settings.Wnode.Guid))) throw std::runtime_error("Cannot create counter session identity");
        wchar_t guid[40]{};
        if (!StringFromGUID2(settings.Wnode.Guid,guid,40)) throw std::runtime_error("Cannot format counter session identity");
        name=L"AnyPS2-Native-"; name+=guid;
        std::copy(name.begin(),name.end(),properties.name);
        settings.Wnode.BufferSize=sizeof(properties); settings.Wnode.Flags=WNODE_FLAG_TRACED_GUID;
        settings.Wnode.ClientContext=1;
        settings.LogFileMode=EVENT_TRACE_REAL_TIME_MODE|EVENT_TRACE_SYSTEM_LOGGER_MODE;
        settings.EnableFlags=EVENT_TRACE_FLAG_CSWITCH;
        settings.BufferSize=64; settings.MinimumBuffers=32; settings.MaximumBuffers=128;
        settings.FlushTimer=1; settings.LoggerNameOffset=offsetof(Properties,name);
        checked(StartTraceW(&session,name.c_str(),&settings),"Start native PMU session (requires Windows tracing permission)");
        checked(TraceSetInformation(session,TracePmcCounterListInfo,sources.data(),sizeof(sources)),"Configure native PMU sources");
        CLASSIC_EVENT_ID event{{0x3d6fa8d1,0xfe05,0x11d0,{0x9d,0xda,0x00,0xc0,0x4f,0xd7,0xba,0x7c}},36,{}};
        checked(TraceSetInformation(session,TracePmcEventListInfo,&event,sizeof(event)),"Configure PMU switch samples");
        LARGE_INTEGER time; QueryPerformanceCounter(&time); beginning=time.QuadPart;
        logfile.LoggerName=name.data(); logfile.Context=this;
        logfile.ProcessTraceMode=PROCESS_TRACE_MODE_REAL_TIME|PROCESS_TRACE_MODE_EVENT_RECORD|PROCESS_TRACE_MODE_RAW_TIMESTAMP;
        logfile.EventRecordCallback=[](EVENT_RECORD* record) {
            auto& self=*static_cast<Impl*>(record->UserContext);
            std::lock_guard lock(self.mutex);
            if (!self.failure.empty() || record->EventHeader.TimeStamp.QuadPart<static_cast<LONGLONG>(self.beginning)) return;
            try {
                if (const auto sample=decode_counter_switch(*record,2)) {
                    self.intervals.consume(sample->view());
                    const auto state=self.intervals.snapshot();
                    if (state.status>=CounterStatus::invalid_sample)
                        self.fail("Invalid native PMU interval stream: "+std::to_string(static_cast<unsigned>(state.status)));
                    if (sample->outgoing_thread==self.owner) self.changed.notify_all();
                }
            } catch (const std::exception& e) { self.fail(e.what()); }
        };
        logfile.BufferCallback=[](EVENT_TRACE_LOGFILEW* buffer)->ULONG {
            auto& self=*static_cast<Impl*>(buffer->Context);
            std::lock_guard lock(self.mutex);
            if (buffer->EventsLost) self.fail("Native PMU events lost");
            return TRUE;
        };
        reader=OpenTraceW(&logfile);
        if (reader==INVALID_PROCESSTRACE_HANDLE) checked(GetLastError(),"Open native PMU consumer");
        pump=std::thread([this] {
            auto handle=reader;
            const auto result=ProcessTrace(&handle,1,nullptr,nullptr);
            std::lock_guard lock(mutex);
            if (result) fail("Native PMU processing failed: "+std::to_string(result));
            ended=true; changed.notify_all();
        });
        flusher=std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested()) {
                Sleep(10);
                Properties flush{}; flush.value.Wnode.BufferSize=sizeof(flush);
                const auto result=FlushTraceW(session,nullptr,&flush.value);
                std::lock_guard lock(mutex);
                if (result) fail("Flush native PMU buffers failed: "+std::to_string(result));
                if (flush.value.EventsLost || flush.value.LogBuffersLost || flush.value.RealTimeBuffersLost)
                    fail("Native PMU buffers lost");
            }
        });
        // Establish a real switch-in baseline before a caller waits for its
        // first complete interval; the initial partial interval is discarded.
        Sleep(1);
    }
    ~Impl() {
        if (flusher.joinable()) flusher.request_stop();
        if (session) {
            properties.value.Wnode.BufferSize=sizeof(properties);
            ControlTraceW(session,nullptr,&properties.value,EVENT_TRACE_CONTROL_STOP);
        }
        if (flusher.joinable()) flusher.join();
        if (reader!=INVALID_PROCESSTRACE_HANDLE) CloseTrace(reader);
        if (pump.joinable()) pump.join();
    }
    std::optional<std::array<std::uint64_t,2>> sample() {
        std::unique_lock lock(mutex);
        if (GetCurrentThreadId()!=owner) { fail("Native PMU sampled from a different thread"); return {}; }
        // Native sampled profiling exposes the last completed measurement.
        // Waiting on every guest transfer would make profiling dominate the
        // display callback and continually create more pending display events.
        // Never extrapolate the running interval or manufacture a zero sample.
        const bool arrived=changed.wait_for(lock,std::chrono::seconds(5),[&] {
            return !failure.empty() || ended || intervals.snapshot().completed_intervals!=0;
        });
        if (!arrived) fail("Timed out waiting for native PMU interval completion");
        if (ended) fail("Native PMU session ended during execution");
        if (!failure.empty()) return {};
        const auto value=intervals.snapshot();
        return std::array{value.totals[0],value.totals[1]};
    }
};
LiveCounters::LiveCounters() : impl_(std::make_unique<Impl>()) { impl_->start(); }
LiveCounters::~LiveCounters()=default;
std::optional<std::array<std::uint64_t,2>> LiveCounters::sample() { return impl_->sample(); }
std::string LiveCounters::error() const { std::lock_guard lock(impl_->mutex); return impl_->failure; }
}
