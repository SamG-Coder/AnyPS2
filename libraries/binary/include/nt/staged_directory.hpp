#pragma once
#include <filesystem>

namespace nt {
// Only this owner's newly created directory is removed on failure. Publish after
// all artifact streams have closed successfully; existing output is never reused.
class StagedDirectory {
public:
    explicit StagedDirectory(const std::filesystem::path& destination);
    StagedDirectory(const StagedDirectory&) = delete;
    StagedDirectory& operator=(const StagedDirectory&) = delete;
    ~StagedDirectory();

    const std::filesystem::path& path() const { return path_; }
    void publish();

private:
    std::filesystem::path destination_, path_;
};
}
