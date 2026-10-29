#include <nt/disc_files.hpp>
#include <algorithm>

namespace nt::disc {
namespace {
bool disc_path(const std::string& path) {
    return (path.starts_with("cdrom0:") || path.starts_with("cdrom:")) && path.find('\0')==std::string::npos;
}
}
std::int32_t Files::mutation_error(const std::string& path) { return disc_path(path) ? -30 : -19; }
FileStat Files::stat(const std::string& path) {
    if (!disc_path(path)) return {-19,{}};
    const auto relative=path.substr(path.find(':')+1);
    if (!relative.empty() && relative.find_first_not_of("/\\")==std::string::npos) return {0,disc_.root()};
    try { return {0,disc_.find(relative)}; }
    catch (const PathError&) { return {-2,{}}; }
}
std::int32_t Files::open(const std::string& path,std::uint32_t mode) {
    if (mode!=1) return -30;
    if (!disc_path(path)) return -19;
    Entry entry;
    try { entry=disc_.find(path.substr(path.find(':')+1)); }
    catch (const PathError&) { return -2; }
    if (entry.directory) return -21;
    if (handles_.size()>=64) return -24;
    std::uint32_t fd=3;
    while (handles_.contains(fd)) ++fd;
    handles_.emplace(fd,Handle{entry}); return static_cast<std::int32_t>(fd);
}
std::int32_t Files::open_directory(const std::string& path) {
    auto result=stat(path);
    if (result.result) return result.result;
    if (!result.entry.directory) return -20;
    if (handles_.size()>=64) return -24;
    auto children=disc_.entries(result.entry);
    std::uint32_t fd=3;
    while (handles_.contains(fd)) ++fd;
    handles_.emplace(fd,Handle{std::move(result.entry),0,std::move(children)});
    return static_cast<std::int32_t>(fd);
}
FileStat Files::read_directory(std::uint32_t fd) {
    const auto found=handles_.find(fd);
    if (found==handles_.end() || !found->second.entry.directory) return {-9,{}};
    auto& handle=found->second;
    if (handle.offset==handle.children.size()) return {0,{}};
    auto entry=handle.children[handle.offset];
    ++handle.offset; return {1,std::move(entry)};
}
std::int32_t Files::close(std::uint32_t fd,bool directory) {
    const auto found=handles_.find(fd);
    if (found==handles_.end() || found->second.entry.directory!=directory) return -9;
    handles_.erase(found); return 0;
}
std::int32_t Files::seek(std::uint32_t fd,std::int32_t offset,std::uint32_t whence) {
    return static_cast<std::int32_t>(seek_bounded(fd,offset,whence,0x7fffffff));
}
std::int64_t Files::seek64(std::uint32_t fd,std::int64_t offset,std::uint32_t whence) {
    return seek_bounded(fd,offset,whence,0x7fffffffffffffffULL);
}
std::int64_t Files::seek_bounded(std::uint32_t fd,std::int64_t offset,std::uint32_t whence,std::uint64_t limit) {
    const auto found=handles_.find(fd);
    if (found==handles_.end()) return -9;
    auto& handle=found->second;
    if (handle.entry.directory) return -21;
    if (whence>2) return -22;
    const auto base=whence==0 ? std::uint64_t(0) : whence==1 ? handle.offset : std::uint64_t(handle.entry.size);
    std::uint64_t position;
    if (offset<0) {
        const auto magnitude=std::uint64_t(0)-static_cast<std::uint64_t>(offset);
        if (magnitude>base) return -22;
        position=base-magnitude;
    } else {
        const auto delta=static_cast<std::uint64_t>(offset);
        if (base>limit || delta>limit-base) return -22;
        position=base+delta;
    }
    if (position>limit) return -22;
    handle.offset=position; return static_cast<std::int64_t>(position);
}
FileRead Files::read(std::uint32_t fd,std::uint32_t size) {
    const auto found=handles_.find(fd);
    if (found==handles_.end()) return {-9,{}};
    if (found->second.entry.directory) return {-21,{}};
    if (size>0x100000) return {-22,{}};
    auto& handle=found->second;
    const auto count=handle.offset>=handle.entry.size ? 0U : std::min(size,handle.entry.size-static_cast<std::uint32_t>(handle.offset));
    auto bytes=count ? disc_.read_range(handle.entry,static_cast<std::uint32_t>(handle.offset),count) : Bytes{};
    handle.offset+=count;
    return {static_cast<std::int32_t>(count),std::move(bytes)};
}
Bytes Files::read_file(const std::string& path,std::uint64_t limit) {
    if (!disc_path(path)) throw PathError("Expected a disc path");
    const auto entry=disc_.find(path.substr(path.find(':')+1));
    if (entry.directory) throw PathError("Expected a disc file");
    return disc_.read(entry,limit);
}
}
