#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <nt/vulkan_renderer.hpp>
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
struct Window {
    HWND handle{};
    ~Window() { if (handle) DestroyWindow(handle); }
};
}

int main() {
    try {
        SetProcessDPIAware();
        WNDCLASSW type{};
        type.lpfnWndProc=DefWindowProcW;
        type.hInstance=GetModuleHandleW(nullptr);
        type.lpszClassName=L"AnyPS2NativePresentationTest";
        if (!RegisterClassW(&type)) throw std::runtime_error("Cannot register test window");
        constexpr unsigned size=256;
        constexpr DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU;
        RECT bounds{0,0,size,size};
        AdjustWindowRect(&bounds,style,FALSE);
        Window window{CreateWindowW(type.lpszClassName,L"AnyPS2 native presentation test",style,
            CW_USEDEFAULT,CW_USEDEFAULT,bounds.right-bounds.left,bounds.bottom-bounds.top,
            nullptr,nullptr,type.hInstance,nullptr)};
        if (!window.handle) throw std::runtime_error("Cannot create test window");
        ShowWindow(window.handle,SW_SHOW);
        nt::graphics::VulkanRenderer renderer(window.handle,true);
        renderer.upload(std::vector<std::uint32_t>(1024*1024));
        nt::graphics::DisplayPlan plan{};
        plan.width=plan.height=size;
        plan.control=1;
        std::uint32_t color=0;
        std::uint64_t finish=0;
        // Reuse both frame slots and every swapchain image repeatedly without
        // draining the queue. Alternate source images and interleave FINISH.
        for (unsigned frame=0;frame<40;++frame) {
            if (frame%10==0) {
                const unsigned widths[]{256,320,192,256},heights[]{256,192,320,256};
                plan.width=widths[frame/10]; plan.height=heights[frame/10];
                RECT resized{0,0,LONG(plan.width),LONG(plan.height)};
                AdjustWindowRect(&resized,style,FALSE);
                if (!SetWindowPos(window.handle,nullptr,0,0,resized.right-resized.left,resized.bottom-resized.top,
                        SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE))
                    throw std::runtime_error("Resize test window");
                renderer.refresh_surface();
            }
            MSG message;
            while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) DispatchMessageW(&message);
            const unsigned base=(frame%2)*256;
            color=0xff000000U|(frame*5U)|((255U-frame*3U)<<8);
            const unsigned pitch=(plan.width+63)/64;
            const nt::graphics::SpriteCommand draw{{0,0,plan.width,plan.height,base,pitch,color,0}};
            renderer.draw(std::span(&draw,1));
            if (frame%7==0) finish=renderer.enqueue_finish();
            plan.surfaces[0]={base,pitch,0,0,plan.width,plan.height};
            if (!renderer.present_display(plan)) throw std::runtime_error("Changed frame was not submitted");
            if (renderer.present_display(plan)) throw std::runtime_error("Unchanged frame was submitted twice");
        }
        const auto pixels=renderer.scanout_readback(plan);
        if (pixels.pixels.size()!=size*size ||
            !std::all_of(pixels.pixels.begin(),pixels.pixels.end(),[&](auto p){return p==color;}))
            throw std::runtime_error("Asynchronous presentation changed source pixels");
        if (renderer.completed_submission()<finish) throw std::runtime_error("FINISH did not retire");
        if (renderer.validation_errors()) throw std::runtime_error(renderer.validation_message());
        std::cout<<"40 native presentations, unchanged-frame rejection, FINISH and readback passed; validation errors=0\n";
    } catch (const std::exception& error) {
        std::cerr<<error.what()<<'\n';
        return 1;
    }
}
