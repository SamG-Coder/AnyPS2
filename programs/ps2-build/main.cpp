#include <nt/process.hpp>
#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
    if (argc<3 || argc>7 || argc%2!=1) {
        std::cerr << "Usage: ps2-build <generated-project> <AnyPS2-source-root> [--vu <vu0.ll>] [--vu1 <vu1.ll>]\n";
        return 2;
    }
    try {
        const auto project = std::filesystem::absolute(argv[1]);
        const auto root = std::filesystem::absolute(argv[2]);
        const auto build = project / "build";
        std::string vu_module,vu1_module;
        for (int index=3;index<argc;index+=2) {
            const std::string_view option=argv[index];
            if (option!="--vu" && option!="--vu1") throw std::invalid_argument("Expected --vu or --vu1");
            auto& module=option=="--vu" ? vu_module : vu1_module;
            if (!module.empty()) throw std::invalid_argument("Duplicate VU module option");
            module=std::filesystem::absolute(argv[index+1]).generic_string();
        }
        auto result = nt::run_process({"cmake", "-S", project.string(), "-B", build.string(),
                                      "-DANYPS2_ROOT=" + root.generic_string(), "-DANYPS2_VU_MODULE="+vu_module,
                                      "-DANYPS2_VU1_MODULE="+vu1_module});
        if (result) return result;
        result = nt::run_process({"cmake", "--build", build.string(), "--config", "Release", "--target", "ps2-native", "--parallel", "8"});
        if (!result) std::cout << "Built diagnostic executable under " << build << ". This is not a playable game build.\n";
        return result;
    } catch (const std::exception& error) {
        std::cerr << "ps2-build: " << error.what() << '\n';
        return 1;
    }
}
