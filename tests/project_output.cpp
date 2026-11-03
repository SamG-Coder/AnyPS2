#include <nt/binary.hpp>
#include <nt/staged_directory.hpp>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class Action> void rejects(Action action) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Invalid output operation succeeded");
}
std::string text(const fs::path& path) {
    const auto bytes = nt::read_file(path);
    return {bytes.begin(), bytes.end()};
}
void publication(const fs::path& root) {
    const auto destination = root / "project";
    const auto unrelated = root / "project.pending-other";
    fs::create_directory(unrelated);
    nt::write_text_file(unrelated / "keep", "unrelated");
    fs::path temporary;
    bool earlier_artifacts_written = false;
    rejects([&] {
        nt::StagedDirectory staged(destination);
        temporary = staged.path();
        nt::write_text_file(temporary / "generated.ll", "; synthetic fixture\n");
        nt::write_text_file(temporary / "module.data", "synthetic data");
        earlier_artifacts_written = true;
        // A directory where a file is required produces a real I/O error after
        // earlier artifacts were written, without platform permissions or hooks.
        fs::create_directory(temporary / "conversion.txt");
        nt::write_text_file(temporary / "conversion.txt", "complete");
        staged.publish();
    });
    check(earlier_artifacts_written && !temporary.empty() && !fs::exists(temporary) && !fs::exists(destination),
          "Failed output left partial artifacts");
    check(text(unrelated / "keep") == "unrelated", "Rollback touched another owner's directory");

    {
        nt::StagedDirectory staged(destination / "");
        temporary = staged.path();
        nt::write_text_file(temporary / "generated.ll", "; retry\n");
        nt::write_text_file(temporary / "conversion.txt", "complete");
        check(!fs::exists(destination), "Output was visible before publication");
        staged.publish();
        rejects([&] { staged.publish(); });
    }
    check(!fs::exists(temporary) && text(destination / "generated.ll") == "; retry\n" &&
          text(destination / "conversion.txt") == "complete", "Retry did not publish complete output");
    rejects([&] { nt::StagedDirectory staged(destination); });
    check(text(destination / "generated.ll") == "; retry\n", "Existing output was modified");

    const auto collision = root / "collision";
    rejects([&] {
        nt::StagedDirectory staged(collision);
        temporary = staged.path();
        nt::write_text_file(temporary / "partial", "owned");
        fs::create_directory(collision);
        nt::write_text_file(collision / "keep", "other publisher");
        staged.publish();
    });
    check(!fs::exists(temporary) && text(collision / "keep") == "other publisher",
          "Publication conflict damaged existing output or left temporary files");

    const auto competing = root / "competing";
    {
        nt::StagedDirectory first(competing), second(competing);
        check(first.path() != second.path(), "Concurrent owners shared a temporary directory");
        temporary = second.path();
        nt::write_text_file(first.path() / "keep", "first publisher");
        first.publish();
        rejects([&] { second.publish(); });
    }
    check(!fs::exists(temporary) && text(competing / "keep") == "first publisher",
          "Competing publication changed completed output");

    const auto file = root / "existing-file";
    nt::write_text_file(file, "existing file");
    rejects([&] { nt::StagedDirectory staged(file); });
    check(text(file) == "existing file", "Existing file was changed");
}
}
int main() {
    const auto root = fs::temp_directory_path() /
        ("anyps2-project-output-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    bool owned = false;
    try {
        owned = fs::create_directory(root);
        check(owned, "Cannot reserve output test directory");
        publication(root);
        fs::remove_all(root);
        std::cout << "Output rollback, retry and publication ownership passed\n";
        return 0;
    } catch (const std::exception& error) {
        if (owned) { std::error_code ignored; fs::remove_all(root, ignored); }
        std::cerr << error.what() << '\n';
        return 1;
    }
}
