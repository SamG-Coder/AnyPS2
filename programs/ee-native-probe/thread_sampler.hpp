#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

namespace probe {
// Opt-in statistical profile of this process's threads, armed for a bounded time.
// Each sample suspends one thread to read its instruction pointer, so while one is
// suspended this code must not allocate: that thread may hold the heap lock.
class ThreadSampler {
public:
    explicit ThreadSampler(std::filesystem::path path) : path_(std::move(path)) {
        samples_.resize(4U*1024U*1024U);
        known_.reserve(256);
        worker_=std::thread([this] { run(); });
    }
    ~ThreadSampler() {
        stopping_=true;
        worker_.join();
        std::ofstream file(path_);
        file<<"base "<<reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr))<<'\n';
        for (std::size_t i=0;i<count_;++i) file<<samples_[i].thread<<' '<<samples_[i].address<<'\n';
    }
    ThreadSampler(const ThreadSampler&)=delete;
    ThreadSampler& operator=(const ThreadSampler&)=delete;
    void arm() { armed_=true; }
private:
    struct Sample { std::uint32_t thread; std::uint64_t address; };
    void refresh() {
        for (const auto handle:known_) CloseHandle(handle.handle);
        known_.clear();
        const auto snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
        if (snapshot==INVALID_HANDLE_VALUE) return;
        THREADENTRY32 entry{}; entry.dwSize=sizeof(entry);
        const auto process=GetCurrentProcessId(),self=GetCurrentThreadId();
        for (auto found=Thread32First(snapshot,&entry);found;found=Thread32Next(snapshot,&entry)) {
            if (entry.th32OwnerProcessID!=process || entry.th32ThreadID==self || known_.size()==known_.capacity()) continue;
            if (const auto handle=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,entry.th32ThreadID))
                known_.push_back({handle,entry.th32ThreadID});
        }
        CloseHandle(snapshot);
    }
    void run() {
        std::chrono::steady_clock::time_point began{},refreshed{};
        bool started=false;
        while (!stopping_) {
            if (!armed_) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); continue; }
            const auto now=std::chrono::steady_clock::now();
            if (!started) { started=true; began=now; }
            if (now-began>std::chrono::seconds(30)) break;
            if (now-refreshed>std::chrono::seconds(1)) { refresh(); refreshed=now; }
            for (const auto& thread:known_) {
                if (count_==samples_.size() || SuspendThread(thread.handle)==DWORD(-1)) continue;
                CONTEXT context{}; context.ContextFlags=CONTEXT_CONTROL;
                const auto read=GetThreadContext(thread.handle,&context);
                ResumeThread(thread.handle);
                if (read) samples_[count_++]={thread.id,context.Rip};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        for (const auto handle:known_) CloseHandle(handle.handle);
        known_.clear();
    }
    struct Known { HANDLE handle; std::uint32_t id; };
    std::filesystem::path path_;
    std::vector<Sample> samples_;
    std::vector<Known> known_;
    std::size_t count_=0;
    std::atomic<bool> armed_{false},stopping_{false};
    std::thread worker_;
};
}
#endif
