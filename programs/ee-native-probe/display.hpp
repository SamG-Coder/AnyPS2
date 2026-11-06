#pragma once
#include <algorithm>
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
    explicit NativeWindow(bool validation=false,unsigned resolution_scale=1,bool fullscreen=false) : fullscreen_requested_(fullscreen),resolution_scale_(resolution_scale) {
#ifdef _WIN32
        SetProcessDPIAware();
        WNDCLASSW wc{};
        wc.lpfnWndProc=[](HWND window,UINT message,WPARAM w,LPARAM l)->LRESULT {
            if (message==WM_CLOSE) { DestroyWindow(window); return 0; }
            const bool alt_enter=w==VK_RETURN && (l&(LPARAM(1)<<29));
            if ((message==WM_KEYDOWN && w==VK_F11) || (message==WM_SYSKEYDOWN && alt_enter)) {
                if (auto* self=reinterpret_cast<NativeWindow*>(GetWindowLongPtrW(window,GWLP_USERDATA)); self && !(l&(LPARAM(1)<<30)))
                    self->fullscreen_requested_=!self->fullscreen_requested_;
                return 0;
            }
            if (message==WM_SYSCHAR && alt_enter) return 0;
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
        SetWindowLongPtrW(window_.handle,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(this));
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
    bool needs_resize(const nt::graphics::DisplayPlan& plan) const {
        return fullscreen_!=fullscreen_requested_ || (fullscreen_ ? !monitor_matches() : width_!=plan.presented_width() || height_!=plan.presented_height());
    }
    void prepare(const nt::graphics::DisplayPlan& plan) {
#ifdef _WIN32
        if (fullscreen_requested_) enter_fullscreen();
        else { leave_fullscreen(); resize(plan.presented_width(),plan.presented_height()); }
#endif
    }
    // Sizes the plan for the window mode: a scaled image shows its detail up to the
    // monitor's size, and full screen fills the monitor at the display's shape.
    void fit(nt::graphics::DisplayPlan& plan) const {
#ifdef _WIN32
        if (resolution_scale_<=1 && !fullscreen_requested_) return;
        const auto shown_width=plan.presented_width(),shown_height=plan.presented_height();
        MONITORINFO monitor{sizeof(monitor)};
        if (!shown_width || !shown_height || !GetMonitorInfoW(MonitorFromWindow(window_.handle,MONITOR_DEFAULTTOPRIMARY),&monitor)) return;
        const auto area=fullscreen_requested_ ? monitor.rcMonitor : monitor.rcWork;
        RECT frame{0,0,0,0};
        if (!fullscreen_requested_) AdjustWindowRect(&frame,style,FALSE);
        const auto limit_width=std::uint32_t(std::max<LONG>(area.right-area.left-(frame.right-frame.left),1));
        const auto limit_height=std::uint32_t(std::max<LONG>(area.bottom-area.top-(frame.bottom-frame.top),1));
        auto height=fullscreen_requested_ ? limit_height : std::min(shown_height*resolution_scale_,limit_height);
        auto width=shown_width*height/shown_height;
        if (width>limit_width) { width=limit_width; height=shown_height*width/shown_width; }
        plan.output_width=width; plan.output_height=height;
        if (fullscreen_requested_) { plan.output_x=(limit_width-width)/2; plan.output_y=(limit_height-height)/2; }
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
        if (!width || !height || width>8192 || height>8192)
            throw std::runtime_error("Invalid native display size");
        RECT bounds{0,0,static_cast<LONG>(width),static_cast<LONG>(height)};
        if (!AdjustWindowRect(&bounds,style,FALSE) ||
            !SetWindowPos(window_.handle,nullptr,0,0,bounds.right-bounds.left,bounds.bottom-bounds.top,
                SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE))
            throw std::runtime_error("Cannot resize native output window");
        renderer_->refresh_surface();
        width_=width; height_=height;
    }
    bool monitor_matches() const {
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(MonitorFromWindow(window_.handle,MONITOR_DEFAULTTOPRIMARY),&monitor)) return true;
        return width_==std::uint32_t(monitor.rcMonitor.right-monitor.rcMonitor.left) && height_==std::uint32_t(monitor.rcMonitor.bottom-monitor.rcMonitor.top);
    }
    // Borderless over the whole monitor, so the desktop's mode stays as it is and
    // nothing changes when focus moves to another window.
    void enter_fullscreen() {
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(MonitorFromWindow(window_.handle,MONITOR_DEFAULTTOPRIMARY),&monitor))
            throw std::runtime_error("Cannot read the monitor for full screen");
        if (!fullscreen_) { GetWindowRect(window_.handle,&windowed_bounds_); ShowCursor(FALSE); }
        SetWindowLongPtrW(window_.handle,GWL_STYLE,WS_POPUP|WS_VISIBLE);
        const auto& area=monitor.rcMonitor;
        if (!SetWindowPos(window_.handle,HWND_TOP,area.left,area.top,area.right-area.left,area.bottom-area.top,SWP_FRAMECHANGED|SWP_SHOWWINDOW))
            throw std::runtime_error("Cannot resize native output window");
        renderer_->refresh_surface();
        width_=area.right-area.left; height_=area.bottom-area.top; fullscreen_=true;
    }
    void leave_fullscreen() {
        if (!fullscreen_) return;
        ShowCursor(TRUE);
        SetWindowLongPtrW(window_.handle,GWL_STYLE,style|WS_VISIBLE);
        if (!SetWindowPos(window_.handle,nullptr,windowed_bounds_.left,windowed_bounds_.top,windowed_bounds_.right-windowed_bounds_.left,
                windowed_bounds_.bottom-windowed_bounds_.top,SWP_FRAMECHANGED|SWP_NOZORDER|SWP_SHOWWINDOW))
            throw std::runtime_error("Cannot resize native output window");
        fullscreen_=false; width_=height_=0;
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
    RECT windowed_bounds_{};
#endif
    bool fullscreen_=false,fullscreen_requested_=false;
    unsigned resolution_scale_=1;
};
}
