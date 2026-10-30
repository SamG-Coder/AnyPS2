#include <nt/process.hpp>
#include <filesystem>
#include <iostream>
#include <map>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

int main(int argc, char** argv) {
    const std::map<std::string,std::string> programs{{"import","ps2-import"},{"inspect","ps2-inspect"},
        {"recompile","ps2-recompile"},{"iop-recompile","ps2-iop-recompile"},{"inventory","ps2-convert-inventory"},
        {"build","ps2-build"}};
    if (argc < 2 || std::string_view(argv[1]) == "--help") {
        std::cout << "AnyPS2 - generic BIOS-free PS2-to-PC toolchain\n"
            "Usage: AnyPS2 <import|inspect|iop-recompile|recompile|inventory|build> [arguments]\n"
            "Each command is a separate C++ program. Run a command without arguments for its usage.\n"
            "LLVM is the only conversion backend. EE entry-flow conversion is experimental and stops at syscalls.\n";
        return 0;
    }
    try {
        const auto command = programs.find(argv[1]);
        if (command == programs.end()) throw std::runtime_error("Unknown AnyPS2 command");
        std::filesystem::path directory;
#ifdef _WIN32
        std::wstring path(32768,L'\0');
        const auto size = GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
        if (!size || size >= path.size()) throw std::runtime_error("Cannot locate AnyPS2 executable");
        path.resize(size); directory = std::filesystem::path(path).parent_path();
        const auto program = directory / (command->second + ".exe");
#else
        directory = std::filesystem::canonical("/proc/self/exe").parent_path();
        const auto program = directory / command->second;
#endif
        std::vector<std::string> arguments{program.string()};
        for (int index=2; index<argc; ++index) arguments.emplace_back(argv[index]);
        return nt::run_process(arguments);
    } catch (const std::exception& error) { std::cerr << "AnyPS2: " << error.what() << '\n'; return 1; }
}
