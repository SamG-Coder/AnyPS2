#pragma once
#include <nt/native_scanout.hpp>
#include <nt/vulkan_renderer.hpp>
#include <memory>
#ifdef _WIN32
#include <windows.h>
#endif

namespace probe {
// Presentation stays on the native execution owner thread. It consumes only
// committed game pixels and keeps the last frame visible at a diagnostic stop.
class NativeWindow {
public:
    NativeWindow() {
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
        window_.handle=CreateWindowW(wc.lpszClassName,L"AnyPS2 - native game output (running)",style,
            CW_USEDEFAULT,CW_USEDEFAULT,bounds.right-bounds.left,bounds.bottom-bounds.top,
            nullptr,nullptr,wc.hInstance,nullptr);
        if (!window_.handle) throw std::runtime_error("Cannot create native output window");
        ShowWindow(window_.handle,SW_SHOW);
        renderer_=std::make_unique<nt::graphics::VulkanRenderer>(window_.handle,true);
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
    void present(const nt::graphics::Scanout& frame) {
#ifdef _WIN32
        if (!IsWindow(window_.handle) || IsIconic(window_.handle)) return;
        if (width_!=frame.width || height_!=frame.height) {
            RECT bounds{0,0,static_cast<LONG>(frame.width),static_cast<LONG>(frame.height)};
            AdjustWindowRect(&bounds,style,FALSE);
            SetWindowPos(window_.handle,nullptr,0,0,bounds.right-bounds.left,bounds.bottom-bounds.top,
                SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
            width_=frame.width; height_=frame.height;
        }
        renderer_->present(frame.pixels,frame.width,frame.height);
        if (renderer_->validation_errors()) throw std::runtime_error("Native output presentation validation failed");
#endif
    }
    void hold(const std::optional<nt::graphics::Scanout>& frame) {
#ifdef _WIN32
        SetWindowTextW(window_.handle,L"AnyPS2 - native game output (execution stopped)");
        while (poll()) {
            if (frame) present(*frame);
            Sleep(16);
        }
#endif
    }
private:
#ifdef _WIN32
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
