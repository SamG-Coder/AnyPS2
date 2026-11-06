#pragma once
#include <atomic>
#include <nt/native_scanout.hpp>
#include <nt/vulkan_renderer.hpp>
#include <memory>
#include <string>
#include <chrono>
#ifdef _WIN32
#include <windows.h>
#endif

namespace probe {
// Presentation stays on the native execution owner thread. It consumes only
// committed game pixels and keeps the last frame visible at a diagnostic stop.
class NativeWindow {
public:
    explicit NativeWindow(bool validation=false,unsigned resolution_scale=1) {
#ifdef _WIN32
        SetProcessDPIAware();
        WNDCLASSW wc{};
        wc.lpfnWndProc=[](HWND window,UINT message,WPARAM w,LPARAM l)->LRESULT {
            if (message==WM_CLOSE) { DestroyWindow(window); return 0; }
            return DefWindowProcW(window,message,w,l);
        };
        wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"AnyPS2NativeCapture";
        if (!RegisterClassW(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)
            throw std::runtime_error("Cannot register native output window");
        RECT bounds{0,0,640,448}; AdjustWindowRect(&bounds,style,FALSE);
        window_.handle=CreateWindowW(wc.lpszClassName,L"AnyPS2 - display updates: 0 (running)",style,
            CW_USEDEFAULT,CW_USEDEFAULT,bounds.right-bounds.left,bounds.bottom-bounds.top,
            nullptr,nullptr,wc.hInstance,nullptr);
        if (!window_.handle) throw std::runtime_error("Cannot create native output window");
        ShowWindow(window_.handle,SW_SHOW);
        renderer_=std::make_unique<nt::graphics::VulkanRenderer>(window_.handle,validation,nt::graphics::RenderPath::native_images,resolution_scale);
#else
        throw std::runtime_error("Native output window is unavailable on this platform");
#endif
    }
    bool poll() {
#ifdef _WIN32
        MSG message;
        while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        return IsWindow(window_.handle)!=0;
#else
        return false;
#endif
    }
    nt::graphics::VulkanRenderer& renderer() { return *renderer_; }
    // Window state belongs to the thread that created the window; presentation
    // itself runs on the render thread (present_display), so the caller never
    // waits for a frame to be recorded or shown.
    bool presentable() const {
#ifdef _WIN32
        return IsWindow(window_.handle) && !IsIconic(window_.handle);
#else
        return false;
#endif
    }
    // A size change recreates the swapchain: the caller must first join the
    // render thread, which is the renderer's only other user.
    bool needs_resize(const nt::graphics::DisplayPlan& plan) const { return width_!=plan.presented_width() || height_!=plan.presented_height(); }
    void prepare(const nt::graphics::DisplayPlan& plan) {
#ifdef _WIN32
        resize(plan.presented_width(),plan.presented_height());
#endif
    }
    // Render thread: shows the plan if it changed the displayed image.
    bool present_display(nt::graphics::VulkanRenderer& renderer,const nt::graphics::DisplayPlan& plan) {
        const bool changed=renderer.present_display(plan);
        if (renderer.validation_errors()) throw std::runtime_error("Native GPU display validation failed");
        if (changed) presented_.fetch_add(1,std::memory_order_relaxed);
        return changed;
    }
    void update_rate() {
#ifdef _WIN32
        const auto now=std::chrono::steady_clock::now();
        const auto elapsed=std::chrono::duration<double>(now-rate_started_).count();
        if (elapsed<1.0) return;
        frames_=presented_.load(std::memory_order_relaxed);
        fps_=static_cast<unsigned>((frames_-rate_frames_)/elapsed+0.5);
        rate_frames_=frames_; rate_started_=now;
        update_title();
#endif
    }
    void present(const nt::graphics::Scanout& frame) {
#ifdef _WIN32
        if (!IsWindow(window_.handle) || IsIconic(window_.handle)) return;
        resize(frame.width,frame.height);
        const auto now=std::chrono::steady_clock::now();
        const bool changed=last_pixels_!=frame.pixels;
        if (!stopped_) {
            const auto elapsed=std::chrono::duration<double>(now-rate_started_).count();
            if (elapsed>=1.0) {
                fps_=static_cast<unsigned>((frames_-rate_frames_)/elapsed+0.5);
                rate_frames_=frames_; rate_started_=now;
                update_title();
            }
            // Re-presenting identical pixels adds FIFO waiting to the execution
            // thread without advancing the game. The swapchain retains them.
            if (!changed) return;
        }
        renderer_->present(frame.pixels,frame.width,frame.height);
        last_pixels_=frame.pixels;
        if (renderer_->validation_errors()) throw std::runtime_error("Native output presentation validation failed");
        if (!stopped_) {
            ++frames_;
            update_title();
        }
#endif
    }
    void hold(const std::optional<nt::graphics::Scanout>& frame) {
#ifdef _WIN32
        stopped_=true; update_title();
        while (poll()) {
            // The last native swapchain image remains visible; no CPU re-upload.
            Sleep(16);
        }
#endif
    }
private:
#ifdef _WIN32
    void resize(unsigned width,unsigned height) {
        if (width_==width && height_==height) return;
        if (!width || !height || width>2048 || height>2048)
            throw std::runtime_error("Invalid native display size");
        RECT bounds{0,0,static_cast<LONG>(width),static_cast<LONG>(height)};
        if (!AdjustWindowRect(&bounds,style,FALSE) ||
            !SetWindowPos(window_.handle,nullptr,0,0,bounds.right-bounds.left,bounds.bottom-bounds.top,
                SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE))
            throw std::runtime_error("Cannot resize native output window");
        renderer_->refresh_surface();
        width_=width; height_=height;
    }
    void update_title() const {
        const auto title=L"AnyPS2 - display updates: "+std::to_wstring(frames_)+
            L" | presentations/s: "+std::to_wstring(stopped_ ? 0 : fps_)+
            (stopped_ ? L" (execution stopped)" : L" (running)");
        SetWindowTextW(window_.handle,title.c_str());
    }
    std::chrono::steady_clock::time_point rate_started_=std::chrono::steady_clock::now();
    std::uint64_t frames_=0,rate_frames_=0;
    std::atomic<std::uint64_t> presented_{0};
    std::vector<std::uint32_t> last_pixels_;
    unsigned fps_=0;
    bool stopped_=false;
    static constexpr DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX;
    struct Window {
        HWND handle=nullptr;
        ~Window() { if (IsWindow(handle)) DestroyWindow(handle); }
    } window_;
    std::unique_ptr<nt::graphics::VulkanRenderer> renderer_;
    unsigned width_=640,height_=448;
#endif
};
}
