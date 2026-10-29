#include <nt/iop_native_heap.hpp>
#include <nt/iop_native_libraries.hpp>
#include <nt/iop_native_boot.hpp>
#include <nt/iop_native_disc.hpp>
#include <nt/iop_native_drivers.hpp>
#include <nt/iop_native_driver_files.hpp>
#include <nt/iop_native_interrupts.hpp>
#include <nt/iop_native_events.hpp>
#include <nt/iop_native_semaphores.hpp>
#include <nt/iop_native_mailboxes.hpp>
#include <nt/iop_native_threads.hpp>
#include <nt/iop_native_system.hpp>
#include <nt/iop_native_alarms.hpp>
#include <nt/iop_native_sif.hpp>
#include <nt/iop_native_sif_control.hpp>
#include <nt/iop_native_sif_commands.hpp>
#include <nt/iop_native_sif_session.hpp>
#include <nt/iop_native_sif_inbound.hpp>
#include <nt/iop_native_rpc.hpp>
#include <nt/iop_native_rpc_data.hpp>
#include <nt/iop_native_stdio.hpp>
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
        nt::iop::NativeHeapServices heap(ram_size,heap_begin,stack_begin);
        nt::iop::NativeLibraryServices libraries(nt_iop_generated_libraries(),nt_iop_generated_imports());
        nt::iop::NativeDriverServices drivers;
        nt::iop::NativeFileDescriptors descriptors;
        nt::iop::NativeDriverFiles driver_files(drivers,descriptors,heap);
        nt::iop::NativeBootServices boot; // No reboot context was supplied to this standalone probe.
        std::unique_ptr<nt::iop::NativeDiscServices> disc;
        nt::iop::NativeAlarmServices alarms;
        nt::iop::NativeSifTransfers sif; // No converted EE data endpoint supplied yet.
        nt::iop::NativeSifCommands commands;
        nt::iop::NativeSifInbound inbound(memory,stack_begin);
        nt::iop::NativeCallbackMux callbacks({&alarms,&sif,&inbound});
        nt::iop::NativeInterruptServices interrupts(0x1f7000,&callbacks);
        nt::iop::NativeSifControl sif_control(interrupts,&sif);
        nt::iop::NativeThreadServices threads(interrupts,&heap);
        nt::iop::NativeRpcServers rpc_servers(nullptr,&threads);
        nt::iop::NativeRpcDataServices rpc_data;
        nt::iop::NativeEventServices events(&threads);
        nt::iop::NativeSemaphoreServices semaphores(&threads);
        nt::iop::NativeMailboxServices mailboxes(&threads);
        nt::iop::NativeSystemServices system(events,boot,memory);
        nt::iop::NativeSifCommandSession command_session(events,system,sif_control);
        inbound.connect(commands,command_session,sif_control,&rpc_servers);
        nt::iop::NativeStdioServices stdio([](std::string_view text) { std::cout.write(text.data(),static_cast<std::streamsize>(text.size())); return bool(std::cout); });
        std::vector<nt::iop::NativeServices*> providers{&drivers,&driver_files,&heap,&libraries,&boot,&interrupts,&events,&semaphores,&mailboxes,&threads,&system,&alarms,&sif,&sif_control,&stdio,&commands,&command_session,&rpc_servers,&rpc_data};
        if (!disc_path.empty()) { disc=std::make_unique<nt::iop::NativeDiscServices>(disc_path,&descriptors); providers.push_back(disc.get()); }
        nt::iop::NativeServiceRouter services(std::move(providers));
        const auto entries=nt_iop_generated_entries();
        if (entries.empty()) throw std::runtime_error("Converted program has no startup entries");
        for (std::size_t index=0;index<entries.size();++index) {
            const auto& entry=entries[index];
            if (entry.base<module.base || entry.base-module.base>module.memory_size ||
                entry.memory_size>module.memory_size-(entry.base-module.base) ||
                entry.initialized_size>entry.memory_size || entry.entry<entry.base || entry.entry-entry.base>=entry.initialized_size)
                throw std::runtime_error("Invalid converted startup entry metadata");
            nt::iop::Cpu state;
            state.pc=entry.entry; state.registers[28]=entry.global_pointer;
            state.registers[29]=stack_top; state.registers[31]=0xfffffffc;
            const auto thread_id=threads.admit(state);
            const auto before=threads.executed();
            nt::iop::NativeResult result{};
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while (true) {
                result=threads.run(nt_iop_generated_run,memory,1000000-(threads.executed()-before),services);
                state=threads.state(thread_id);
                if (result.reason==nt::iop::NativeStop::unmapped_code && result.pc==0xfffffffc && state.pc!=0xfffffffc) continue;
                const auto now=std::chrono::steady_clock::now();
                if (result.reason==nt::iop::NativeStop::thread_wait && interrupts.delivery_enabled() && now<deadline) {
                    auto delay=alarms.delay();
                    if (const auto thread_delay=threads.delay(); thread_delay && (!delay || *thread_delay<*delay)) delay=thread_delay;
                    if (delay) {
                        std::this_thread::sleep_for(std::min(*delay,std::chrono::duration_cast<std::chrono::nanoseconds>(deadline-now)));
                        continue;
                    }
                }
                break;
            }
            if (result.reason==nt::iop::NativeStop::unmapped_code && state.pc==0xfffffffc && !state.delay_slot) {
                std::cout<<"module="<<index<<" returned="<<state.registers[2]<<" executed="<<state.executed<<'\n';
                continue;
            }
            std::cerr<<"Converted IOP program stopped: module="<<index<<" reason="<<static_cast<unsigned>(result.reason)
                     <<" pc="<<std::hex<<state.pc<<" address="<<result.address
                     <<" trap_code="<<result.trap_code<<" delay_slot="<<state.delay_slot<<'\n';
            return 3;
        }
        return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
