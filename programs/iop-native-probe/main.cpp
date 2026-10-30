#include <nt/iop_native_startup.hpp>
#include <thread>
#include <memory>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#ifdef ANYPS2_IOP_LLVM
#include <nt/iop_llvm.hpp>
static nt::iop::NativeResult nt_iop_generated_run(nt::iop::Cpu& cpu,std::span<std::uint8_t> ram,std::uint64_t budget) {
    return nt::iop::run_llvm(cpu,ram,budget);
}
#else
nt::iop::NativeResult nt_iop_generated_run(nt::iop::Cpu&,std::span<std::uint8_t>,std::uint64_t);
#endif
nt::iop::NativeModule nt_iop_generated_module();
std::vector<nt::iop::NativeModule> nt_iop_generated_entries();
std::vector<nt::iop::NativeLibrarySpec> nt_iop_generated_libraries();
std::vector<nt::iop::NativeImportSpec> nt_iop_generated_imports();

int main(int argc,char** argv) {
    try {
        std::filesystem::path data_path,disc_path;
        for (int at=1;at<argc;++at) {
            if (std::string_view(argv[at])=="--disc" && at+1<argc && disc_path.empty()) disc_path=argv[++at];
            else if (data_path.empty() && !std::string_view(argv[at]).starts_with("--")) data_path=argv[at];
            else throw std::runtime_error("Usage: iop-native [module.data] [--disc image.iso]");
        }
        const auto module=nt_iop_generated_module();
        constexpr std::uint32_t ram_size=2*1024*1024, stack_begin=0x1f0000, stack_top=0x1ff000;
        if (module.base>=stack_begin || module.memory_size>stack_begin-module.base ||
            module.initialized_size>module.memory_size || module.entry<module.base ||
            module.entry-module.base>=module.initialized_size)
            throw std::runtime_error("Converted module overlaps the host invocation stack or has invalid metadata");
        const auto path=!data_path.empty() ? data_path : std::filesystem::absolute(argv[0]).parent_path()/"module.data";
        if (std::filesystem::file_size(path)!=module.initialized_size) throw std::runtime_error("Module data size mismatch");
        std::vector<std::uint8_t> memory(ram_size);
        std::ifstream file(path,std::ios::binary);
        if (!file.read(reinterpret_cast<char*>(memory.data()+module.base),module.initialized_size))
            throw std::runtime_error("Cannot read converted module data");
        const auto heap_begin=(module.base+module.memory_size+255)&~255U;
        nt::iop::NativeServiceContext services(memory,{heap_begin,stack_begin},
            nt_iop_generated_libraries(),nt_iop_generated_imports(),
            [](std::string_view text) { std::cout.write(text.data(),static_cast<std::streamsize>(text.size())); return bool(std::cout); },disc_path);
        auto& threads=services.threads;
        auto& interrupts=services.interrupts;
        auto& alarms=services.alarms;
        nt::iop::NativeModuleStartup startup(nt_iop_generated_run,services,memory,module,nt_iop_generated_entries(),stack_top);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        std::size_t reported=0;
        while (!startup.finished()) {
            const auto result=startup.advance(1000000-startup.executed());
            for (;reported<startup.returned().size();++reported) {
                const auto& returned=startup.returned()[reported];
                std::cout<<"module="<<reported<<" returned="<<returned.value<<" executed="<<returned.executed<<'\n';
            }
            if (startup.finished()) break;
            const auto now=std::chrono::steady_clock::now();
            if (result.reason==nt::iop::NativeStop::thread_wait && interrupts.delivery_enabled() && now<deadline) {
                auto delay=alarms.delay();
                if (const auto thread_delay=threads.delay(); thread_delay && (!delay || *thread_delay<*delay)) delay=thread_delay;
                if (delay) {
                    std::this_thread::sleep_for(std::min(*delay,std::chrono::duration_cast<std::chrono::nanoseconds>(deadline-now)));
                    continue;
                }
            }
            std::cerr<<"Native startup stopped: module="<<reported<<" reason="<<static_cast<unsigned>(result.reason)
                     <<" pc="<<std::hex<<result.pc<<" address="<<result.address<<" trap_code="<<result.trap_code<<'\n';
            return 3;
        }
        std::cout<<"startup_callbacks=complete executed="<<startup.callback_operations()<<'\n';
        const auto workers_before=threads.executed();
        while (true) {
            const auto workers=threads.run(nt_iop_generated_run,memory,1000000-(threads.executed()-workers_before),services);
            if (workers.reason==nt::iop::NativeStop::unmapped_code && workers.pc==0xfffffffc) continue;
            if (workers.reason==nt::iop::NativeStop::thread_wait) {
                std::cout<<"ready_workers=drained executed="<<threads.executed()-workers_before<<'\n';
                break;
            }
            std::cerr<<"Native worker stopped: reason="<<static_cast<unsigned>(workers.reason)
                     <<" pc="<<std::hex<<workers.pc<<" address="<<workers.address<<" trap_code="<<workers.trap_code<<'\n';
            return 3;
        }
        return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
