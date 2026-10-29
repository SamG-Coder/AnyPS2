#include <nt/process.hpp>
#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: ps2-build <generated-project> <AnyPS2-source-root>\n";
        return 2;
    }
    try {
        const auto project = std::filesystem::absolute(argv[1]);
        const auto root = std::filesystem::absolute(argv[2]);
        const auto build = project / "build";
        auto result = nt::run_process({"cmake", "-S", project.string(), "-B", build.string(),
                                      "-DANYPS2_ROOT=" + root.generic_string()});
        if (result) return result;
        result = nt::run_process({"cmake", "--build", build.string(), "--config", "Release", "--target", "ps2-native", "--parallel", "8"});
        if (!result) std::cout << "Built diagnostic executable under " << build << ". This is not a playable game build.\n";
        return result;
    } catch (const std::exception& error) {
        std::cerr << "ps2-build: " << error.what() << '\n';
        return 1;
    }
}
