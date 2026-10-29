#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <nt/vulkan_renderer.hpp>
#include <iostream>
#include <vector>
#include <string_view>

namespace {
LRESULT CALLBACK messages(HWND window,UINT message,WPARAM w,LPARAM l) {
    if (message==WM_CLOSE) {PostQuitMessage(0);return 0;}
    return DefWindowProcW(window,message,w,l);
}
struct Window {
    HWND handle{};
    ~Window(){if(handle) DestroyWindow(handle);}
};
}
int main(int argc,char** argv) {
    try {
        const bool smoke=argc==2 && std::string_view(argv[1])=="--smoke";
        if (argc>1 && !smoke) throw std::runtime_error("Usage: anyps2-display [--smoke]");
        SetProcessDPIAware();
        WNDCLASSW wc{};wc.lpfnWndProc=messages;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"AnyPS2Vulkan";
        if (!RegisterClassW(&wc)) throw std::runtime_error("Cannot register window");
        constexpr unsigned width=640,height=448;
        constexpr DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX;
        RECT bounds{0,0,width,height};AdjustWindowRect(&bounds,style,FALSE);
        Window window{CreateWindowW(wc.lpszClassName,L"AnyPS2 — Vulkan synthetic GS output",style,CW_USEDEFAULT,CW_USEDEFAULT,
            bounds.right-bounds.left,bounds.bottom-bounds.top,nullptr,nullptr,wc.hInstance,nullptr)};
        if (!window.handle) throw std::runtime_error("Cannot create window");
        ShowWindow(window.handle,SW_SHOW);
        nt::graphics::VulkanRenderer renderer(window.handle,true);
        std::vector<std::uint32_t> memory(1024*1024,0xff18120e), pixels(width*height);
        renderer.sprite(memory,{32,32,608,416,0,10,0xff384828,0});
        renderer.sprite(memory,{64,64,320,384,0,10,0xffdda32a,0});
        renderer.sprite(memory,{320,64,576,384,0,10,0xff407ee8,0});
        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){
            const unsigned block=((x>>3)&1)|(((y>>3)&1)<<1)|(((x>>4)&1)<<2)|(((y>>4)&1)<<3)|(((x>>5)&1)<<4);
            const unsigned word=(x&1)|((y&1)<<1)|((x&6)<<1)|((y&6)<<3);
            pixels[y*width+x]=memory[((y/32)*10+x/64)*2048+block*64+word];
        }
        bool running=true;unsigned frames=0;
        while(running){
            MSG msg;while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){if(msg.message==WM_QUIT)running=false;TranslateMessage(&msg);DispatchMessageW(&msg);}
            if (!running) break;
            if (IsIconic(window.handle)) {Sleep(16);continue;}
            renderer.present(pixels,width,height);
            if(smoke && ++frames==3)break;
        }
        if(renderer.validation_errors())throw std::runtime_error("Vulkan validation errors during presentation");
        std::cout<<"Vulkan compute and presentation: "<<renderer.device_name()<<"; validation errors=0\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
