#pragma once
#include <nt/iso9660.hpp>
#include <map>

namespace nt::disc {
struct FileStat { std::int32_t result; Entry entry; };
struct FileRead { std::int32_t result; Bytes bytes; };
// Shared read-only disc handles. Paths never resolve against the host filesystem.
class Files {
public:
    explicit Files(const std::filesystem::path& image) : disc_(image) {}
    std::int32_t open(const std::string& path,std::uint32_t mode);
    std::int32_t close(std::uint32_t fd,bool directory=false);
    std::int32_t open_directory(const std::string& path);
    FileStat read_directory(std::uint32_t fd);
    bool directory(std::uint32_t fd) const { return handles_.contains(fd) && handles_.at(fd).entry.directory; }
    std::int32_t seek(std::uint32_t fd,std::int32_t offset,std::uint32_t whence);
    static std::int32_t mutation_error(const std::string& path);
    std::int64_t seek64(std::uint32_t fd,std::int64_t offset,std::uint32_t whence);
    FileStat stat(const std::string& path);
    FileRead read(std::uint32_t fd,std::uint32_t count);
    bool contains(std::uint32_t fd) const { return handles_.contains(fd); }
    Bytes read_file(const std::string& path,std::uint64_t limit);
    void reset() { handles_.clear(); }
private:
    struct Handle { Entry entry; std::uint64_t offset=0; std::vector<Entry> children; };
    std::int64_t seek_bounded(std::uint32_t fd,std::int64_t offset,std::uint32_t whence,std::uint64_t limit);
    Iso9660 disc_;
    std::map<std::uint32_t,Handle> handles_;
};
}
