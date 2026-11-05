#pragma once
#include <nt/native_drawing.hpp>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace nt::graphics {
// Owned native drawing jobs recorded in submission order by one worker thread.
// Callers may execute independent compiled work while jobs are queued, but must
// join (wait or drain) before touching drawing/renderer. At most queue_limit
// jobs and queued_bytes_limit bytes of packets are outstanding (a single larger
// job is accepted into an empty queue); submit blocks only when the queue is
// full, so bursts of small batches overlap recording instead of stalling. The
// referenced objects must outlive this worker. Destruction records accepted
// jobs and joins without throwing; normal execution retrieves errors through
// poll() or wait().
//
// enqueue() adds renderer work, such as presentation, ordered after every job
// accepted before it. The caller keeps executing while the frame is recorded
// and presented, as a game thread does with its render thread.
class NativeDrawingWorker {
public:
    using Duration=std::chrono::steady_clock::duration;
    static constexpr std::size_t queue_limit=64,queued_bytes_limit=64U*1024U*1024U;
    NativeDrawingWorker(NativeDrawing& drawing,VulkanRenderer& renderer)
        :drawing_(drawing),renderer_(renderer),thread_([this] { run(); }) {}
    ~NativeDrawingWorker() {
        { std::lock_guard lock(mutex_); stopping_=true; }
        changed_.notify_all();
        thread_.join();
    }
    NativeDrawingWorker(const NativeDrawingWorker&)=delete;
    NativeDrawingWorker& operator=(const NativeDrawingWorker&)=delete;
    void submit(std::span<const Packet> packets) {
        const auto bytes=validate(packets);
        push({std::vector<Packet>(packets.begin(),packets.end()),{},bytes});
    }
    // Takes ownership without copying; packets are unchanged if this throws.
    void submit(std::vector<Packet>&& packets) {
        const auto bytes=validate(packets);
        std::unique_lock lock(mutex_);
        changed_.wait(lock,[&] { return error_ || room(bytes); });
        rethrow();
        jobs_.push_back({std::move(packets),{},bytes});
        queued_bytes_+=bytes;
        lock.unlock();
        changed_.notify_all();
    }
    // The task runs on the worker thread, which alone uses the renderer until
    // the caller joins. A throwing task fails the worker like a failed job.
    void enqueue(std::function<void(VulkanRenderer&)> task) {
        if (!task) throw std::invalid_argument("Native drawing task is empty");
        push({{},std::move(task)});
    }
    bool poll() {
        std::lock_guard lock(mutex_);
        rethrow();
        return jobs_.empty() && !active_;
    }
    void wait() {
        std::unique_lock lock(mutex_);
        changed_.wait(lock,[&] { return error_ || (jobs_.empty() && !active_); });
        rethrow();
    }
    // VIF command-drain boundaries await parsing/recording and enqueue the
    // Vulkan work, without waiting for raster completion on the host.
    void drain() { wait(); renderer_.enqueue_finish(); }
    Duration elapsed() const { std::lock_guard lock(mutex_); return elapsed_; }
private:
    // Returns the batch size in bytes.
    static std::size_t validate(std::span<const Packet> packets) {
        if (packets.size()>maximum_batch_bytes/sizeof(Quadword))
            throw std::invalid_argument("Native drawing job has too many packets");
        std::size_t words=0;
        for (const auto& packet:packets) {
            if (packet.size()>maximum_batch_bytes/sizeof(Quadword)-words)
                throw std::invalid_argument("Native drawing job exceeds batch bound");
            words+=packet.size();
        }
        return words*sizeof(Quadword);
    }
    bool room(std::size_t bytes) const {
        if (jobs_.size()+active_>=queue_limit) return false;
        return (jobs_.empty() && !active_) || bytes<=queued_bytes_limit-std::min(queued_bytes_,queued_bytes_limit);
    }
    struct Job {
        std::vector<Packet> packets;
        std::function<void(VulkanRenderer&)> task;
        std::size_t bytes=0;
    };
    void push(Job job) {
        std::unique_lock lock(mutex_);
        changed_.wait(lock,[&] { return error_ || room(job.bytes); });
        rethrow();
        queued_bytes_+=job.bytes;
        jobs_.push_back(std::move(job));
        lock.unlock();
        changed_.notify_all();
    }
    void rethrow() { if (error_) std::rethrow_exception(error_); }
    void run() {
        std::unique_lock lock(mutex_);
        for (;;) {
            changed_.wait(lock,[&] { return stopping_ || (!jobs_.empty() && !error_); });
            // Destruction records every accepted job unless one has failed.
            if (jobs_.empty() || error_) return;
            auto job=std::move(jobs_.front());
            jobs_.pop_front(); active_=1; queued_bytes_-=job.bytes;
            lock.unlock();
            const auto started=std::chrono::steady_clock::now();
            std::exception_ptr failure;
            try {
                if (job.task) job.task(renderer_);
                else drawing_.submit(renderer_,job.packets);
            }
            catch (...) { failure=std::current_exception(); }
            const auto finished=std::chrono::steady_clock::now();
            lock.lock();
            elapsed_+=finished-started; active_=0;
            // A failed job stops later queued jobs; they are discarded unrecorded.
            if (failure) { error_=failure; jobs_.clear(); queued_bytes_=0; }
            changed_.notify_all();
        }
    }
    NativeDrawing& drawing_;
    VulkanRenderer& renderer_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Job> jobs_;
    std::size_t active_=0,queued_bytes_=0;
    bool stopping_=false;
    std::exception_ptr error_;
    Duration elapsed_{};
    std::thread thread_;
};
}
