#include <nt/process.hpp>
#include <stdexcept>
#ifdef _WIN32
#include <process.h>
#else
#include <cerrno>
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

namespace nt {
int run_process(const std::vector<std::string>& arguments) {
    if (arguments.empty()) throw std::runtime_error("Empty process argument list");
#ifdef _WIN32
    // The CRT serializes argv for CreateProcess. Quote each argument, escaping
    // backslashes before quotes/end, so paths with spaces remain one argument.
    std::vector<std::string> quoted;
    for (const auto& arg : arguments) {
        std::string result = "\"";
        std::size_t slashes = 0;
        for (char c : arg) {
            if (c == '\\') { ++slashes; continue; }
            result.append(slashes * (c == '"' ? 2 : 1), '\\');
            slashes = 0;
            if (c == '"') result += '\\';
            result += c;
        }
        result.append(slashes * 2, '\\'); result += '"';
        quoted.push_back(std::move(result));
    }
    std::vector<const char*> argv;
    for (const auto& arg : quoted) argv.push_back(arg.c_str());
    argv.push_back(nullptr);
    const auto status = _spawnvp(_P_WAIT, arguments[0].c_str(), argv.data());
    if (status == -1) throw std::runtime_error("Cannot launch process: " + arguments[0]);
    return static_cast<int>(status);
#else
    std::vector<char*> argv;
    for (const auto& arg : arguments) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    pid_t pid;
    if (posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ))
        throw std::runtime_error("Cannot launch process: " + arguments[0]);
    int status;
    while (waitpid(pid, &status, 0) == -1) if (errno != EINTR) throw std::runtime_error("Cannot wait for process");
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
#endif
}
}
