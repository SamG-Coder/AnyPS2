#pragma once
#include <nt/native_drawing.hpp>
#include <future>

namespace nt::graphics {
// A single owned native drawing job. Callers may execute independent compiled
// work while it records commands, but must join before touching drawing/renderer.
// The referenced objects must outlive this worker. Destruction joins without
// throwing; normal execution retrieves errors through poll() or wait().
class NativeDrawingWorker {
public:
    using Duration=std::chrono::steady_clock::duration;
    NativeDrawingWorker(NativeDrawing& drawing,VulkanRenderer& renderer):drawing_(drawing),renderer_(renderer) {}
    ~NativeDrawingWorker() { if (job_.valid()) job_.wait(); }
    void submit(std::span<const Packet> packets) {
        wait();
        if (packets.size()>maximum_batch_bytes/sizeof(Quadword))
            throw std::invalid_argument("Native drawing job has too many packets");
        std::size_t words=0;
        for (const auto& packet:packets) {
            if (packet.size()>maximum_batch_bytes/sizeof(Quadword)-words)
                throw std::invalid_argument("Native drawing job exceeds batch bound");
            words+=packet.size();
        }
        std::vector<Packet> owned(packets.begin(),packets.end());
        job_=std::async(std::launch::async,[this,owned=std::move(owned)] {
            const auto started=std::chrono::steady_clock::now();
            drawing_.submit(renderer_,owned);
            return std::chrono::steady_clock::now()-started;
        });
    }
    bool poll() {
        if (job_.valid() && job_.wait_for(std::chrono::seconds(0))!=std::future_status::ready) return false;
        wait(); return true;
    }
    void wait() {
        if (error_) std::rethrow_exception(error_);
        if (job_.valid()) {
            try { elapsed_+=job_.get(); }
            catch (...) { error_=std::current_exception(); throw; }
        }
    }
    Duration elapsed() const { return elapsed_; }
private:
    NativeDrawing& drawing_;
    VulkanRenderer& renderer_;
    std::future<Duration> job_;
    std::exception_ptr error_;
    Duration elapsed_{};
};
}
