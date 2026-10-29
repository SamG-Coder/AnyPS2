#include <nt/elf.hpp>
#include <nt/runtime.hpp>
#include <nt/kernel.hpp>
#include <nt/system_bus.hpp>
#include <charconv>
#include <iostream>
#include <string_view>
#include <fstream>

nt::runtime::Result nt_generated_run(nt::runtime::State&, nt::runtime::Bus&, std::uint64_t);
std::uint64_t nt_generated_identity();
std::uint32_t nt_generated_entry();

int main(int argc, char** argv) {
    try {
        bool raw = false;
        std::filesystem::path capture;
        std::filesystem::path disc;
        for (int index = 3; index < argc; ++index) {
            const std::string_view option(argv[index]);
            if (option == "--raw") raw = true;
            else if (option.starts_with("--capture=")) {
                capture = std::string(option.substr(10));
                if (capture.empty()) throw std::runtime_error("Capture path is empty");
            } else if (option.starts_with("--disc=")) {
                disc = std::string(option.substr(7));
                if (disc.empty()) throw std::runtime_error("Disc path is empty");
            } else throw std::runtime_error("Usage: ps2-native [boot.elf] [budget] [--raw] [--capture=NEW_DIRECTORY] [--disc=IMAGE.iso]");
        }
        if (!capture.empty() && std::filesystem::exists(capture)) throw std::runtime_error("Capture directory already exists");
        std::uint64_t budget = 1000000;
        if (argc >= 3) {
            const std::string_view value(argv[2]);
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), budget);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || budget == 0)
                throw std::runtime_error("Budget must be a positive integer");
        }
        const auto input = argc > 1 ? std::filesystem::path(argv[1]) :
            std::filesystem::absolute(argv[0]).parent_path() / "boot.elf";
        const auto bytes = nt::read_file(input);
        if (nt::fingerprint(bytes) != nt_generated_identity()) throw std::runtime_error("ELF does not match generated code");
        const auto elf = nt::elf::parse(bytes);
        nt::hardware::SystemBus bus;
        if (!disc.empty()) bus.sif().mount_disc(disc);
        for (const auto& segment : elf.segments)
            bus.install(segment.address, nt::ByteView(bytes).subspan(segment.offset, segment.file_size), segment.memory_size);
        nt::runtime::State state;
        if (!raw) nt::kernel::Kernel::initialize_cpu(state);
        state.pc = nt_generated_entry();
        nt::kernel::Kernel kernel;
        const auto execution = raw ? nt::kernel::RunResult{nt_generated_run(state,bus,budget),0} :
            nt::kernel::run(nt_generated_run,state,bus,kernel,budget);
        const auto result = execution.stop;
        const auto fault_address = bus.last_address;
        const auto fault_size = bus.last_size;
        const auto fault_write = bus.last_write;
        if (!result.detail.empty()) std::cout << "detail=" << result.detail << '\n';
        std::cout << "AnyPS2 diagnostic native execution (BIOS-free; partial hardware runtime)\n"
                  << "stop=" << nt::runtime::name(result.reason) << " pc=" << nt::hex32(result.pc)
                  << " word=" << nt::hex32(result.word) << " executed=" << state.executed
                  << " delay_slot=" << state.delay_slot << '\n';
        std::cout << "hle_calls=" << execution.serviced_calls << " thread=" << kernel.main_thread().id
                  << " stack=" << nt::hex32(kernel.main_thread().stack_base)
                  << " heap_end=" << nt::hex32(kernel.main_thread().heap_end) << '\n';
        if (state.delay_slot) std::cout << "branch_pc=" << nt::hex32(state.branch_pc) << '\n';
        std::cout << "completed_interrupts=" << kernel.completed_interrupts()
                  << " interrupt_active=" << kernel.interrupt_active()
                  << " pending_cpu=" << nt::hex32(bus.interrupt_lines())
                  << " dma_status=" << nt::hex32(static_cast<std::uint32_t>(bus.load(0x1000e010,4)))
                  << " iop_packets=" << bus.sif().iop_received_packets() << '\n';
        if (bus.sif().rpc_bind_requests())
            std::cout << "rpc_bind_requests=" << bus.sif().rpc_bind_requests()
                      << " last_rpc_bind_server=" << nt::hex32(bus.sif().last_rpc_bind_server()) << '\n';
        const auto fileio = bus.sif().fileio_statistics();
        if (!disc.empty()) std::cout << "fileio_requests=" << fileio.requests << " fileio_opens=" << fileio.opens
                                    << " fileio_bytes_read=" << fileio.bytes_read << " fileio_last_method=" << fileio.last_method << '\n';
        if (result.reason == nt::runtime::Stop::memory_fault)
            std::cout << "access=" << (fault_write ? "write" : "read") << " address=" << nt::hex32(fault_address) << " bytes=" << fault_size << '\n';
        for (unsigned i = 0; i < 32; ++i)
            std::cout << "r" << i << " lo=0x" << std::hex << state.gpr[i].lo << " hi=0x" << state.gpr[i].hi << std::dec << '\n';
        if (!capture.empty()) {
            if (!std::filesystem::create_directories(capture)) throw std::runtime_error("Capture directory already exists");
            nt::write_file(capture / "ram.bin",bus.snapshot());
            std::ofstream identity(capture / "image-id.txt");
            identity << nt_generated_identity() << '\n'; identity.close();
            if (!identity) throw std::runtime_error("Cannot finish capture identity");
            std::ofstream roots(capture / "observed-entries.txt");
            if (result.reason == nt::runtime::Stop::unmapped_code || result.reason == nt::runtime::Stop::code_changed)
                roots << nt::hex32(result.pc) << '\n';
            for (unsigned number = 0; number < 256; ++number) {
                const auto address = kernel.handler(number,bus);
                if (address) roots << nt::hex32(address) << '\n';
            }
            roots.close();
            if (!roots) throw std::runtime_error("Cannot finish observed entries");
            std::cout << "capture=" << capture << '\n';
        }
        // A diagnostic stop is never reported as successfully running the game.
        return result.reason == nt::runtime::Stop::budget ? 4 : 3;
    } catch (const std::exception& error) {
        std::cerr << "ps2-native: " << error.what() << '\n';
        return 1;
    }
}
