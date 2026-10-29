#include <nt/process.hpp>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    const std::vector<std::string> values{"with spaces", "quote\"inside", "trailing slash\\", "", "literal $() & ; ` text"};
    if (argc > 1) {
        if (argc != static_cast<int>(values.size()) + 2 || std::string(argv[1]) != "child") return 11;
        for (std::size_t i = 0; i < values.size(); ++i) if (argv[i + 2] != values[i]) return 12;
        return 0;
    }
    try {
        std::vector<std::string> args{argv[0], "child"};
        args.insert(args.end(), values.begin(), values.end());
        return nt::run_process(args);
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
