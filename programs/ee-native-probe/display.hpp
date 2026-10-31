#pragma once
#include <nt/native_scanout.hpp>
#include <nt/vulkan_renderer.hpp>
#ifdef _WIN32
#include <windows.h>
#endif

namespace probe {
// Explicit diagnostic view of the last game-produced surface after execution
// stops. Closing this window ends inspection; it does not resume game execution.
inline void display_stopped(const nt::graphics::Scanout& frame) {
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
    constexpr DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX;
    RECT bounds{0,0,static_cast<LONG>(frame.width),static_cast<LONG>(frame.height)};
    AdjustWindowRect(&bounds,style,FALSE);
    struct Window { HWND handle; ~Window() { if (IsWindow(handle)) DestroyWindow(handle); } };
    Window window{CreateWindowW(wc.lpszClassName,L"AnyPS2 - native game output (execution stopped)",style,
        CW_USEDEFAULT,CW_USEDEFAULT,bounds.right-bounds.left,bounds.bottom-bounds.top,
        nullptr,nullptr,wc.hInstance,nullptr)};
    if (!window.handle) throw std::runtime_error("Cannot create native output window");
    ShowWindow(window.handle,SW_SHOW);
    nt::graphics::VulkanRenderer renderer(window.handle,true);
    while (IsWindow(window.handle)) {
        MSG message;
        while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        if (!IsWindow(window.handle)) break;
        if (!IsIconic(window.handle)) renderer.present(frame.pixels,frame.width,frame.height);
        Sleep(16);
    }
    if (renderer.validation_errors()) throw std::runtime_error("Native output presentation validation failed");
#else
    throw std::runtime_error("Native output window is unavailable on this platform");
#endif
}
}
