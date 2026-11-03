#include <nt/staged_directory.hpp>
#include <chrono>
#include <stdexcept>
#include <string>

namespace nt {
StagedDirectory::StagedDirectory(const std::filesystem::path& destination) {
    if (destination.empty()) throw std::invalid_argument("Output directory is empty");
    destination_ = std::filesystem::absolute(destination).lexically_normal();
    if (!destination_.has_filename()) destination_ = destination_.parent_path();
    if (std::filesystem::exists(std::filesystem::symlink_status(destination_)))
        throw std::runtime_error("Output directory already exists");
    const auto parent = destination_.parent_path();
    std::filesystem::create_directories(parent);
    const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        auto candidate = parent / (destination_.filename().string() + ".pending-" + stamp + "-" + std::to_string(attempt));
        if (std::filesystem::create_directory(candidate)) {
            path_.swap(candidate);
            return;
        }
    }
    throw std::runtime_error("Cannot reserve a conversion output directory");
}

StagedDirectory::~StagedDirectory() {
    if (!path_.empty()) {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
}

void StagedDirectory::publish() {
    if (path_.empty()) throw std::logic_error("Output directory was already published");
    if (std::filesystem::exists(std::filesystem::symlink_status(destination_)))
        throw std::runtime_error("Output directory already exists");
    std::filesystem::rename(path_, destination_);
    path_.clear();
}
}
