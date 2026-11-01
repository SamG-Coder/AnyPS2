#include <nt/iso9660.hpp>
#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace nt::disc {
namespace {
constexpr std::uint64_t sector_size = 2048;
std::string normalize(std::string value) {
    value = value.substr(0, value.find(';'));
    for (auto& c : value) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return value;
}
}
Iso9660::Iso9660(const std::filesystem::path& path)
    : file_(path, std::ios::binary), size_(std::filesystem::file_size(path)) {
    if (!file_) throw std::runtime_error("Cannot open disc image");
    // Volume descriptors may start with a boot record. Bound the search and
    // explicitly reject raw BIN/CHD rather than treating them as sector images.
    for (std::uint64_t sector = 16; sector < 80; ++sector) {
        const auto descriptor = read_at(sector * sector_size, sector_size);
        if (std::string(descriptor.begin() + 1, descriptor.begin() + 6) != "CD001" || descriptor[6] != 1)
            throw std::runtime_error("Expected a 2048-byte-sector ISO9660 image");
        if (descriptor[0] == 255) break;
        if (descriptor[0] != 1) continue;
        if (le16(descriptor, 128) != sector_size || descriptor[130] != 8 || descriptor[131] != 0)
            throw std::runtime_error("Unsupported ISO logical block size");
        volume_.assign(descriptor.begin() + 40, descriptor.begin() + 72);
        while (!volume_.empty() && volume_.back() == ' ') volume_.pop_back();
        root_ = record(ByteView(descriptor).subspan(156, descriptor[156]));
        if (!root_.directory) throw std::runtime_error("ISO root is not a directory");
        if (le32(descriptor,80)==be32(descriptor,84)) volume_sectors_=le32(descriptor,80);
        return;
    }
    throw std::runtime_error("Missing ISO primary volume descriptor");
}
Bytes Iso9660::read_at(std::uint64_t offset, std::uint64_t size) {
    if (offset > size_ || size > size_ - offset) throw std::runtime_error("Disc extent exceeds image bounds");
    Bytes data(static_cast<std::size_t>(size));
    file_.seekg(static_cast<std::streamoff>(offset));
    if (!file_.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size)))
        throw std::runtime_error("Cannot read disc extent");
    return data;
}
Entry Iso9660::record(ByteView b) {
    require_range(b, 0, 34);
    if (b[0] != b.size() || 33U + b[32] > b.size()) throw std::runtime_error("Invalid directory record");
    if (b[1] || b[26] || b[27] || (b[25] & 128))
        throw std::runtime_error("Extended attributes, interleaving and multi-extent files are unsupported");
    if (le32(b, 2) != be32(b, 6) || le32(b, 10) != be32(b, 14))
        throw std::runtime_error("ISO extent byte orders disagree");
    Entry entry{std::string(b.begin() + 33, b.begin() + 33 + b[32]), le32(b, 2), le32(b, 10), bool(b[25] & 2)};
    std::copy_n(b.begin()+18,7,entry.recorded.begin());
    return entry;
}
Bytes Iso9660::read(const Entry& entry, std::uint64_t limit) {
    if (entry.size > limit) throw std::runtime_error("Disc file exceeds read limit");
    return read_at(std::uint64_t(entry.sector) * sector_size, entry.size);
}
Bytes Iso9660::read_range(const Entry& entry, std::uint32_t offset, std::uint32_t count) {
    if (offset > entry.size || count > entry.size - offset)
        throw std::runtime_error("Disc read exceeds file bounds");
    return read_at(std::uint64_t(entry.sector) * sector_size + offset,count);
}
Bytes Iso9660::read_sectors(std::uint32_t first,std::uint32_t count) {
    if (count>65536) throw std::runtime_error("Disc sector read exceeds 128 MiB limit");
    return read_at(std::uint64_t(first)*sector_size,std::uint64_t(count)*sector_size);
}
std::uint32_t Iso9660::appended_volume_start() {
    if (appended_start_) return *appended_start_;
    if (size_%sector_size || !volume_sectors_ || volume_sectors_>sector_count())
        throw std::runtime_error("Invalid ISO volume size for layer discovery");
    if (volume_sectors_==sector_count()) { appended_start_=0; return 0; }
    const auto descriptor=read_sectors(volume_sectors_,1);
    if (descriptor[0]!=1 || std::string(descriptor.begin()+1,descriptor.begin()+6)!="CD001" || descriptor[6]!=1 ||
        le16(descriptor,128)!=sector_size || descriptor[130]!=8 || descriptor[131]!=0 ||
        !le32(descriptor,80) || le32(descriptor,80)!=be32(descriptor,84))
        throw std::runtime_error("Unrecognized appended ISO volume descriptor");
    appended_start_=volume_sectors_;
    return *appended_start_;
}
std::vector<Entry> Iso9660::entries(const Entry& directory) {
    if (!directory.directory) throw std::runtime_error("Expected a directory");
    const auto data = read(directory, 16 * 1024 * 1024);
    std::vector<Entry> result;
    for (std::size_t pos = 0; pos < data.size();) {
        const auto length = data[pos];
        if (!length) { pos = (pos / sector_size + 1) * sector_size; continue; }
        require_range(data, pos, length);
        if (pos % sector_size + length > sector_size) throw std::runtime_error("ISO record crosses sector boundary");
        auto entry = record(ByteView(data).subspan(pos, length));
        pos += length;
        if (entry.name.size() == 1 && (entry.name[0] == 0 || entry.name[0] == 1)) continue;
        result.push_back(std::move(entry));
    }
    return result;
}
Entry Iso9660::find(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    while (!path.empty() && path.front() == '/') path.erase(path.begin());
    if (path.empty() || path.back() == '/') throw PathError("Invalid ISO path");
    std::istringstream parts(path);
    std::string part;
    auto current = root_;
    while (std::getline(parts, part, '/')) {
        if (part.empty() || part == "." || part == ".." || !current.directory) throw PathError("Invalid ISO path");
        auto children = entries(current);
        std::vector<Entry> matches;
        for (const auto& child : children)
            if (normalize(child.name) == normalize(part)) matches.push_back(child);
        if (matches.empty()) throw PathError("ISO path missing: " + path);
        if (matches.size() != 1) throw std::runtime_error("Ambiguous ISO path: " + path);
        current = matches.front();
    }
    return current;
}
std::string boot_path(ByteView system_cnf) {
    std::istringstream text(std::string(system_cnf.begin(), system_cnf.end()));
    const std::regex pattern(R"(^\s*BOOT2\s*=\s*cdrom\d*:[\\/]*([^\r\n]+))", std::regex::icase);
    std::string line;
    while (std::getline(text, line)) {
        std::smatch match;
        if (!std::regex_search(line, match, pattern)) continue;
        auto path = match[1].str();
        while (!path.empty() && std::isspace(static_cast<unsigned char>(path.back()))) path.pop_back();
        if (path.empty()) break;
        return path;
    }
    throw std::runtime_error("SYSTEM.CNF has no recognized PS2 BOOT2 path");
}
}
