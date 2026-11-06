#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace nt::iop {
// Memory cards as ordinary host files, above the card transport boundary. Each card
// is a directory holding one host file or directory per card entry, plus a manifest
// with the exact names and attributes a host file system cannot keep: card names are
// case-sensitive bytes and host names are sanitized. A file reaches the host when it
// is closed or flushed, as a temporary file renamed over the old one, so a crash
// leaves the previous version. Pages, ECC, FAT and authentication do not exist here.
class NativeMemoryCardFiles {
public:
    static constexpr int succeed=0,changed_card=-1,full_device=-3,no_entry=-4,denied=-5,not_empty=-6,handle_limit=-7,no_card=-12;
    static constexpr std::uint16_t readable=1,writeable=2,executable=4,copy_protected=8,attribute_file=0x10,subdirectory=0x20,
        create_directory=0x40,create_file=0x200,attribute_0400=0x400,hidden=0x2000,exists=0x8000;
    // The capacity a formatted 8 MB card advertises. A real card's count depends on
    // its bad blocks; games only compare it against what they need to store.
    static constexpr std::uint32_t cluster_bytes=1024,allocatable_clusters=8000;
    static constexpr std::size_t entry_bytes=64;
    using ListedEntry=std::array<std::uint8_t,entry_bytes>;

    void set_root(std::filesystem::path root) { root_=std::move(root); }
    const std::string& error() const { return error_; }
    // Reports a newly seen card once, as a console does after a card is inserted.
    int detect(unsigned port,unsigned slot,bool report_change) {
        const auto ready=attach(port,slot);
        if (ready) return ready;
        auto& card=*cards_[port];
        if (report_change && !card.reported) { card.reported=true; return changed_card; }
        return succeed;
    }
    int card_type(unsigned port,unsigned slot) { return attach(port,slot) ? 0 : 2; }
    int format_state(unsigned port,unsigned slot) { return attach(port,slot) ? 0 : 1; }
    int free_clusters(unsigned port,unsigned slot) {
        if (attach(port,slot)) return 0;
        return static_cast<int>(free_clusters(*cards_[port]));
    }
    int open(unsigned port,unsigned slot,std::string_view name,std::uint32_t flags) {
        if (const auto ready=attach(port,slot)) return ready;
        if (flags&create_file) flags|=writeable;
        if (name.empty()) return no_entry;
        const auto free_handle=std::find_if(handles_.begin(),handles_.end(),[](const auto& handle) { return !handle.entry; });
        if (free_handle==handles_.end()) return handle_limit;
        const int fd=static_cast<int>(free_handle-handles_.begin());
        auto& card=*cards_[port];
        const auto found=find(card,name);
        if (!found.parent) return no_entry;
        const bool want_read=flags&readable,want_write=flags&writeable;
        if (found.entry) {
            auto& entry=*found.entry;
            if (flags&create_directory) return no_entry;
            if ((want_write && !(entry.mode&writeable)) || (want_read && !(entry.mode&readable))) return denied;
            if (entry.mode&subdirectory) {
                if (!(entry.mode&readable)) return denied;
                *free_handle=Handle{&entry,port,0,false,false};
                return fd;
            }
            if (want_write && std::any_of(handles_.begin(),handles_.end(),[&](const auto& other) { return other.entry==&entry && other.write; }))
                return denied;
            if (flags&create_file) {
                entry.data.clear(); entry.mode=new_mode(flags,attribute_file); entry.created=entry.modified=now(); entry.dirty=true;
            }
            *free_handle=Handle{&entry,port,0,want_read,want_write};
            return fd;
        }
        if (!(flags&(create_file|create_directory))) return no_entry;
        const bool directory=flags&create_directory;
        const auto parent_size=found.parent->children.size()+2;
        const auto needed=directory_clusters(parent_size+1)-directory_clusters(parent_size)+(directory ? 1U : 0U);
        if (needed>free_clusters(card)) return full_device;
        if (found.leaf.empty() || found.leaf.size()>31) return no_entry;
        auto created=std::make_unique<Entry>();
        created->name=found.leaf; created->host=unique_host_name(*found.parent,found.leaf); created->parent=found.parent;
        created->mode=new_mode(flags,directory ? subdirectory : attribute_file);
        created->created=created->modified=now(); created->dirty=true;
        auto* entry=created.get();
        found.parent->children.push_back(std::move(created));
        if (!store(card,*entry)) { found.parent->children.pop_back(); return no_card; }
        if (directory) return succeed;
        *free_handle=Handle{entry,port,0,want_read,want_write};
        return fd;
    }
    int close(int fd) {
        auto* handle=valid_handle(fd);
        if (!handle) return denied;
        const auto committed=commit(*handle);
        *handle={};
        return committed;
    }
    int close_all() {
        int result=succeed;
        for (auto& handle:handles_) if (handle.entry) {
            if (const auto committed=commit(handle)) result=committed;
            handle={};
        }
        return result;
    }
    int flush(int fd) {
        auto* handle=valid_handle(fd);
        if (!handle) return denied;
        const auto committed=commit(*handle);
        if (committed) *handle={};
        return committed;
    }
    int read(int fd,std::span<std::uint8_t> output) {
        auto* handle=valid_handle(fd);
        if (!handle || !handle->read) return denied;
        const auto& data=handle->entry->data;
        if (handle->position>=data.size()) return 0;
        const auto count=std::min<std::size_t>(output.size(),data.size()-handle->position);
        std::copy_n(data.begin()+handle->position,count,output.begin());
        handle->position+=static_cast<std::uint32_t>(count);
        return static_cast<int>(count);
    }
    // Writes what fits and reports a full card only when nothing does.
    int write(int fd,std::span<const std::uint8_t> input) {
        auto* handle=valid_handle(fd);
        if (!handle || !handle->write) return denied;
        if (input.empty()) return 0;
        auto& entry=*handle->entry;
        const auto owned=file_clusters(entry.data.size());
        const std::uint64_t room=std::uint64_t(owned+free_clusters(*cards_[handle->port]))*cluster_bytes;
        if (handle->position>=room) return full_device;
        const auto count=std::min<std::uint64_t>(input.size(),room-handle->position);
        const auto end=handle->position+count;
        if (end>entry.data.size()) entry.data.resize(end);
        std::copy_n(input.begin(),count,entry.data.begin()+handle->position);
        handle->position=static_cast<std::uint32_t>(end);
        entry.dirty=true;
        return static_cast<int>(count);
    }
    int seek(int fd,std::int32_t offset,std::int32_t origin) {
        auto* handle=valid_handle(fd);
        if (!handle) return denied;
        const std::int64_t base=origin==0 ? 0 : origin==2 ? std::int64_t(handle->entry->data.size()) : std::int64_t(handle->position);
        const auto target=std::clamp<std::int64_t>(base+offset,0,0x7fffffff);
        handle->position=static_cast<std::uint32_t>(target);
        return static_cast<int>(target);
    }
    // Starts (flags zero) or continues one card-wide listing. A listing is a snapshot,
    // so entries deleted while it runs still appear, unlike on a card.
    int list(unsigned port,unsigned slot,std::string_view path,std::uint32_t flags,std::uint32_t maximum,std::vector<ListedEntry>& output) {
        if (const auto ready=attach(port,slot)) return ready;
        if (!(flags&0xffff)) {
            listing_.clear(); listing_valid_=false;
            const auto separator=path.rfind('/');
            const auto directory=separator==std::string_view::npos ? std::string_view{} : separator==0 ? std::string_view("/") : path.substr(0,separator);
            const auto pattern=separator==std::string_view::npos ? path : path.substr(separator+1);
            auto& card=*cards_[port];
            auto* folder=directory.empty() ? card.current : find(card,directory).entry;
            if (!folder || !(folder->mode&subdirectory)) return no_entry;
            const auto add=[&](const Entry& entry,std::string_view shown) {
                if ((entry.mode&hidden) || !matches(pattern,shown)) return;
                listing_.push_back(listed(entry,shown));
            };
            if (folder!=card.root.get()) { add(*folder,"."); add(folder->parent ? *folder->parent : *folder,".."); }
            for (const auto& child:folder->children) add(*child,child->name);
            listing_valid_=true; listing_next_=0;
        }
        if (!listing_valid_) return no_entry;
        int count=0;
        while (maximum-- && listing_next_<listing_.size()) { output.push_back(listing_[listing_next_++]); ++count; }
        return count;
    }
    // Only an existing entry can be deleted: flags select restoring a deleted one,
    // and deleted entries are not kept.
    int remove(unsigned port,unsigned slot,std::string_view path,std::uint32_t flags) {
        if (const auto ready=attach(port,slot)) return ready;
        auto& card=*cards_[port];
        const auto found=find(card,path);
        if (!found.entry || flags || found.entry==card.root.get() || found.leaf.empty() || found.leaf=="." || found.leaf=="..") return no_entry;
        auto& entry=*found.entry;
        if (!(entry.mode&writeable)) return denied;
        if ((entry.mode&subdirectory) && !entry.children.empty()) return not_empty;
        if (std::any_of(handles_.begin(),handles_.end(),[&](const auto& handle) { return handle.entry==&entry; })) return denied;
        std::error_code failure;
        if (entry.mode&subdirectory) std::filesystem::remove(host_path(card,entry)/manifest_name,failure);
        std::filesystem::remove(host_path(card,entry),failure);
        if (failure) return fail("Cannot delete "+host_path(card,entry).string()+": "+failure.message());
        if (card.current==&entry) card.current=entry.parent;
        auto& siblings=entry.parent->children;
        auto* parent=entry.parent;
        siblings.erase(std::find_if(siblings.begin(),siblings.end(),[&](const auto& child) { return child.get()==&entry; }));
        return write_manifest(card,*parent) ? succeed : no_card;
    }
    int change_directory(unsigned port,unsigned slot,std::string_view path,std::string& current) {
        if (const auto ready=attach(port,slot)) return ready;
        auto& card=*cards_[port];
        auto* entry=find(card,path).entry;
        if (!entry || !(entry->mode&subdirectory)) return no_entry;
        card.current=entry;
        current.clear();
        for (const auto* at=entry;at->parent;at=at->parent) current="/"+at->name+current;
        if (current.empty()) current="/";
        return succeed;
    }
    // flags select fields: 1 created, 2 modified, 4 and 0x200 attributes, 8 protection
    // data, 0x10 name. Only the attribute bits a program may change are taken.
    int set_info(unsigned port,unsigned slot,std::string_view path,std::span<const std::uint8_t,entry_bytes> info,std::uint32_t flags) {
        if (const auto ready=attach(port,slot)) return ready;
        auto& card=*cards_[port];
        const auto found=find(card,path);
        if (!found.entry || found.entry==card.root.get()) return no_entry;
        auto& entry=*found.entry;
        std::string renamed;
        if (flags&attribute_file) {
            const auto* text=reinterpret_cast<const char*>(info.data()+32);
            renamed.assign(text,strnlen(text,31));
            if (renamed.empty() || renamed=="." || renamed==".." || renamed.find('/')!=std::string::npos ||
                std::any_of(entry.parent->children.begin(),entry.parent->children.end(),[&](const auto& child) { return child->name==renamed; }))
                return no_entry;
        }
        if (flags&copy_protected) std::memcpy(&entry.reserve2,info.data()+24,4);
        if (flags&(executable|create_file)) {
            std::uint16_t attributes; std::memcpy(&attributes,info.data()+20,2);
            entry.mode=static_cast<std::uint16_t>((entry.mode&~0x180fU)|(attributes&0x180fU));
        }
        if (flags&readable) std::copy_n(info.begin(),8,entry.created.begin());
        if (flags&writeable) std::copy_n(info.begin()+8,8,entry.modified.begin());
        if (flags&attribute_file) entry.name=std::move(renamed);
        return write_manifest(card,*entry.parent) ? succeed : no_card;
    }
    // Directory entries share clusters in pairs, so an odd count leaves one free slot.
    int entry_space(unsigned port,unsigned slot,std::string_view path) {
        if (const auto ready=attach(port,slot)) return ready;
        const auto* entry=find(*cards_[port],path).entry;
        if (!entry) return no_entry;
        return static_cast<int>((entry->children.size()+2)&1);
    }
    // Keeps the replaced contents beside the card rather than destroying saves.
    int format(unsigned port,unsigned slot) {
        if (const auto ready=attach(port,slot)) return ready;
        for (auto& handle:handles_) if (handle.entry && handle.port==port) handle={};
        listing_.clear(); listing_valid_=false;
        const auto directory=card_directory(port);
        std::error_code failure;
        for (unsigned attempt=1;attempt<1000;++attempt) {
            auto kept=directory; kept+=".before-format-"+std::to_string(attempt);
            if (std::filesystem::exists(kept,failure)) continue;
            std::filesystem::rename(directory,kept,failure);
            if (failure) return fail("Cannot set aside "+directory.string()+": "+failure.message());
            const bool reported=cards_[port]->reported;
            cards_[port].reset();
            if (const auto ready=attach(port,slot)) return ready;
            cards_[port]->reported=reported;
            return succeed;
        }
        return fail("Too many earlier formats beside "+directory.string());
    }
private:
    using Time=std::array<std::uint8_t,8>;
    static constexpr std::string_view manifest_name=".anyps2-card";
    static constexpr std::string_view temporary_suffix=".anyps2tmp";
    struct Entry {
        std::string name,host;
        std::uint16_t mode=0,reserve1=0;
        std::uint32_t reserve2=0;
        Time created{},modified{};
        std::vector<std::uint8_t> data;
        std::vector<std::unique_ptr<Entry>> children;
        Entry* parent=nullptr;
        bool dirty=false;
    };
    struct Card {
        std::unique_ptr<Entry> root;
        Entry* current=nullptr;
        unsigned port=0;
        bool reported=false;
    };
    struct Handle {
        Entry* entry=nullptr;
        unsigned port=0;
        std::uint32_t position=0;
        bool read=false,write=false;
    };
    struct Found {
        Entry* entry=nullptr;
        Entry* parent=nullptr;
        std::string leaf;
    };
    static std::uint32_t file_clusters(std::size_t bytes) { return static_cast<std::uint32_t>((bytes+cluster_bytes-1)/cluster_bytes); }
    static std::uint32_t directory_clusters(std::size_t entries) { return static_cast<std::uint32_t>((entries+1)/2); }
    static std::uint16_t new_mode(std::uint32_t flags,std::uint16_t kind) {
        return static_cast<std::uint16_t>((flags&hidden)|readable|writeable|executable|kind|attribute_0400|exists);
    }
    static Time now() {
        // Cards keep the console's clock, which is Japan Standard Time.
        using namespace std::chrono;
        const auto moment=floor<seconds>(system_clock::now())+hours{9};
        const auto day=floor<days>(moment);
        const year_month_day date{day};
        const hh_mm_ss time{moment-day};
        const auto year=static_cast<unsigned>(static_cast<int>(date.year()));
        return {0,static_cast<std::uint8_t>(time.seconds().count()),static_cast<std::uint8_t>(time.minutes().count()),
                static_cast<std::uint8_t>(time.hours().count()),static_cast<std::uint8_t>(unsigned(date.day())),
                static_cast<std::uint8_t>(unsigned(date.month())),static_cast<std::uint8_t>(year&255),static_cast<std::uint8_t>(year>>8)};
    }
    static std::uint32_t used_clusters(const Entry& entry) {
        if (!(entry.mode&subdirectory)) return file_clusters(entry.data.size());
        auto total=directory_clusters(entry.children.size()+2);
        for (const auto& child:entry.children) total+=used_clusters(*child);
        return total;
    }
    static std::uint32_t free_clusters(const Card& card) {
        const auto used=used_clusters(*card.root);
        return used>=allocatable_clusters ? 0 : allocatable_clusters-used;
    }
    static bool matches(std::string_view pattern,std::string_view name) {
        if (pattern.empty()) return name.empty();
        if (pattern.front()=='*') {
            const auto rest=pattern.substr(1);
            for (std::size_t skip=0;skip<=name.size();++skip) if (matches(rest,name.substr(skip))) return true;
            return false;
        }
        if (name.empty()) return false;
        return (pattern.front()=='?' || pattern.front()==name.front()) && matches(pattern.substr(1),name.substr(1));
    }
    static ListedEntry listed(const Entry& entry,std::string_view shown) {
        ListedEntry out{};
        std::copy(entry.created.begin(),entry.created.end(),out.begin());
        std::copy(entry.modified.begin(),entry.modified.end(),out.begin()+8);
        const auto size=(entry.mode&subdirectory) ? 0U : static_cast<std::uint32_t>(entry.data.size());
        std::memcpy(out.data()+16,&size,4);
        std::memcpy(out.data()+20,&entry.mode,2);
        std::memcpy(out.data()+22,&entry.reserve1,2);
        std::copy_n(shown.begin(),std::min<std::size_t>(shown.size(),31),out.begin()+32);
        return out;
    }
    static std::string hex(std::span<const std::uint8_t> bytes) {
        static constexpr char digits[]="0123456789abcdef";
        std::string out;
        for (const auto byte:bytes) { out.push_back(digits[byte>>4]); out.push_back(digits[byte&15]); }
        return out;
    }
    static std::optional<std::vector<std::uint8_t>> unhex(std::string_view text) {
        if (text.size()%2) return {};
        const auto digit=[](char c) { return c>='0' && c<='9' ? c-'0' : c>='a' && c<='f' ? c-'a'+10 : -1; };
        std::vector<std::uint8_t> out;
        for (std::size_t index=0;index<text.size();index+=2) {
            const auto high=digit(text[index]),low=digit(text[index+1]);
            if (high<0 || low<0) return {};
            out.push_back(static_cast<std::uint8_t>(high*16+low));
        }
        return out;
    }
    static std::string lower(std::string text) {
        std::transform(text.begin(),text.end(),text.begin(),[](unsigned char c) { return static_cast<char>(c>='A' && c<='Z' ? c+32 : c); });
        return text;
    }
    // A manifest is user-editable data: only a single plain name may become a path.
    static bool plain_host_name(std::string_view name) {
        return !name.empty() && name.size()<=64 && name!="." && name!=".." && name!=manifest_name &&
            name.find_first_of("/\\:")==std::string_view::npos && !name.ends_with(temporary_suffix);
    }
    static std::string unique_host_name(const Entry& parent,std::string_view name) {
        std::string base;
        for (const unsigned char c:name)
            base.push_back((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='-' || c=='_' || c=='.' ? char(c) : '_');
        while (!base.empty() && base.back()=='.') base.pop_back();
        if (base.empty() || base.front()=='.') base.insert(base.begin(),'_');
        if (base.ends_with(temporary_suffix)) base.push_back('_');
        const auto stem=lower(base.substr(0,base.find('.')));
        static constexpr std::array<std::string_view,4> reserved{"con","prn","aux","nul"};
        const bool numbered=stem.size()==4 && stem[3]>='0' && stem[3]<='9' && (stem.starts_with("com") || stem.starts_with("lpt"));
        if (numbered || (stem.size()==3 && std::find(reserved.begin(),reserved.end(),stem)!=reserved.end())) base.insert(base.begin(),'_');
        const auto taken=[&](const std::string& candidate) {
            return std::any_of(parent.children.begin(),parent.children.end(),[&](const auto& child) { return lower(child->host)==lower(candidate); });
        };
        auto candidate=base;
        for (unsigned suffix=1;taken(candidate);++suffix) candidate=base+"~"+std::to_string(suffix);
        return candidate;
    }
    int fail(std::string message) { error_=std::move(message); return no_card; }
    Handle* valid_handle(int fd) {
        if (fd<0 || fd>=static_cast<int>(handles_.size()) || !handles_[fd].entry) return nullptr;
        return &handles_[fd];
    }
    std::filesystem::path card_directory(unsigned port) const { return root_/("slot"+std::to_string(port+1)); }
    std::filesystem::path host_path(const Card& card,const Entry& entry) const {
        if (!entry.parent) return card_directory(card.port);
        return host_path(card,*entry.parent)/entry.host;
    }
    Found find(Card& card,std::string_view path) {
        Found found;
        auto* at=path.starts_with('/') ? card.root.get() : card.current;
        found.parent=at->parent ? at->parent : at;
        found.entry=at;
        while (!path.empty()) {
            const auto separator=path.find('/');
            const auto part=path.substr(0,separator);
            path=separator==std::string_view::npos ? std::string_view{} : path.substr(separator+1);
            if (part.empty()) continue;
            if (!found.entry || !(found.entry->mode&subdirectory)) return Found{};
            found.leaf=std::string(part);
            if (part==".") { found.parent=found.entry->parent ? found.entry->parent : found.entry; continue; }
            if (part=="..") {
                found.entry=found.entry->parent ? found.entry->parent : found.entry;
                found.parent=found.entry->parent ? found.entry->parent : found.entry;
                continue;
            }
            found.parent=found.entry;
            const auto child=std::find_if(found.parent->children.begin(),found.parent->children.end(),[&](const auto& item) { return item->name==part; });
            found.entry=child==found.parent->children.end() ? nullptr : child->get();
        }
        return found;
    }
    int commit(const Handle& handle) {
        if (!handle.entry->dirty) return succeed;
        handle.entry->modified=now();
        return store(*cards_[handle.port],*handle.entry) ? succeed : no_card;
    }
    bool store(Card& card,Entry& entry) {
        std::error_code failure;
        const auto path=host_path(card,entry);
        if (entry.mode&subdirectory) {
            std::filesystem::create_directory(path,failure);
            if (failure) { fail("Cannot create "+path.string()+": "+failure.message()); return false; }
        } else {
            auto temporary=path; temporary+=temporary_suffix;
            {
                std::ofstream output(temporary,std::ios::binary|std::ios::trunc);
                output.write(reinterpret_cast<const char*>(entry.data.data()),static_cast<std::streamsize>(entry.data.size()));
                output.flush();
                if (!output) { fail("Cannot write "+temporary.string()); return false; }
            }
            std::filesystem::rename(temporary,path,failure);
            if (failure) { fail("Cannot replace "+path.string()+": "+failure.message()); return false; }
        }
        entry.dirty=false;
        return write_manifest(card,*entry.parent);
    }
    bool write_manifest(const Card& card,const Entry& directory) {
        const auto path=host_path(card,directory)/manifest_name;
        auto temporary=path; temporary+=temporary_suffix;
        {
            std::ofstream output(temporary,std::ios::binary|std::ios::trunc);
            for (const auto& child:directory.children) {
                const std::span name(reinterpret_cast<const std::uint8_t*>(child->name.data()),child->name.size());
                output<<hex(name)<<' '<<child->host<<' '<<child->mode<<' '<<child->reserve1<<' '<<child->reserve2<<' '
                      <<hex(child->created)<<' '<<hex(child->modified)<<'\n';
            }
            output.flush();
            if (!output) { fail("Cannot write "+temporary.string()); return false; }
        }
        std::error_code failure;
        std::filesystem::rename(temporary,path,failure);
        if (failure) { fail("Cannot replace "+path.string()+": "+failure.message()); return false; }
        return true;
    }
    // Entries come from the manifest; a host file it does not list, such as one copied
    // in by hand, appears under its own name with default attributes.
    bool load_directory(Entry& directory,const std::filesystem::path& path,unsigned depth,std::size_t& count,std::uint64_t& bytes) {
        if (depth>16) { fail("Memory card folders are nested too deeply: "+path.string()); return false; }
        std::error_code failure;
        std::ifstream manifest(path/manifest_name,std::ios::binary);
        struct Listed { std::string name; std::uint32_t mode,reserve1,reserve2; Time created,modified; };
        std::vector<std::pair<std::string,Listed>> listed_entries;
        for (std::string line;std::getline(manifest,line);) {
            std::istringstream fields(line);
            std::string name,host,created,modified; Listed item{};
            fields>>name>>host>>item.mode>>item.reserve1>>item.reserve2>>created>>modified;
            const auto decoded=unhex(name);
            const auto first=unhex(created),second=unhex(modified);
            if (!fields || !decoded || decoded->empty() || decoded->size()>31 || !first || !second || first->size()!=8 || second->size()!=8 || !plain_host_name(host)) {
                fail("Invalid memory card manifest: "+(path/manifest_name).string()); return false;
            }
            item.name.assign(decoded->begin(),decoded->end());
            std::copy(first->begin(),first->end(),item.created.begin());
            std::copy(second->begin(),second->end(),item.modified.begin());
            listed_entries.emplace_back(std::move(host),std::move(item));
        }
        std::vector<std::filesystem::path> present;
        for (const auto& item:std::filesystem::directory_iterator(path,failure)) present.push_back(item.path());
        if (failure) { fail("Cannot read "+path.string()+": "+failure.message()); return false; }
        std::sort(present.begin(),present.end());
        const auto load_entry=[&](const std::filesystem::path& item,const std::string& host,const Listed* known) {
            const auto status=std::filesystem::symlink_status(item,failure);
            if (failure || std::filesystem::is_symlink(status) || (!std::filesystem::is_regular_file(status) && !std::filesystem::is_directory(status)))
                return true;
            if (++count>4096) { fail("Too many memory card entries in "+path.string()); return false; }
            const bool folder=std::filesystem::is_directory(status);
            auto entry=std::make_unique<Entry>();
            entry->host=host; entry->parent=&directory;
            entry->name=known ? known->name : host;
            entry->mode=known ? static_cast<std::uint16_t>(known->mode) : new_mode(0,folder ? subdirectory : attribute_file);
            entry->mode=static_cast<std::uint16_t>((entry->mode&~(attribute_file|subdirectory))|exists|(folder ? subdirectory : attribute_file));
            if (known) { entry->reserve1=static_cast<std::uint16_t>(known->reserve1); entry->reserve2=known->reserve2; entry->created=known->created; entry->modified=known->modified; }
            else entry->created=entry->modified=now();
            if (entry->name.size()>31) { fail("Memory card name is too long: "+item.string()); return false; }
            if (folder) {
                if (!load_directory(*entry,item,depth+1,count,bytes)) return false;
            } else {
                const auto size=std::filesystem::file_size(item,failure);
                bytes+=failure ? 0 : size;
                if (failure || bytes>std::uint64_t(allocatable_clusters)*cluster_bytes) {
                    fail("Memory card contents exceed its capacity: "+item.string()); return false;
                }
                entry->data.resize(size);
                std::ifstream input(item,std::ios::binary);
                input.read(reinterpret_cast<char*>(entry->data.data()),static_cast<std::streamsize>(size));
                if (!input) { fail("Cannot read "+item.string()); return false; }
            }
            directory.children.push_back(std::move(entry));
            return true;
        };
        for (const auto& [host,item]:listed_entries) {
            const auto target=path/host;
            if (std::find(present.begin(),present.end(),target)==present.end()) continue;
            if (!load_entry(target,host,&item)) return false;
        }
        for (const auto& item:present) {
            const auto host=item.filename().string();
            if (host==manifest_name || host.ends_with(temporary_suffix) ||
                std::any_of(listed_entries.begin(),listed_entries.end(),[&](const auto& known) { return known.first==host; })) continue;
            if (!plain_host_name(host)) continue;
            if (!load_entry(item,host,nullptr)) return false;
        }
        return true;
    }
    // Loads a card on first use. Only port 1 and 2 of a console exist here, with no
    // multitap slots; asking for anything else finds no card, as on a console.
    int attach(unsigned port,unsigned slot) {
        if (port>=cards_.size() || slot || root_.empty()) return no_card;
        if (cards_[port]) return succeed;
        const auto directory=card_directory(port);
        std::error_code failure;
        std::filesystem::create_directories(directory,failure);
        if (failure) return fail("Cannot create "+directory.string()+": "+failure.message());
        auto card=std::make_unique<Card>();
        card->port=port;
        card->root=std::make_unique<Entry>();
        card->root->mode=new_mode(0,subdirectory);
        card->current=card->root.get();
        std::size_t count=0; std::uint64_t bytes=0;
        if (!load_directory(*card->root,directory,0,count,bytes)) return no_card;
        if (used_clusters(*card->root)>allocatable_clusters) return fail("Memory card contents exceed its capacity: "+directory.string());
        cards_[port]=std::move(card);
        return succeed;
    }
    std::filesystem::path root_;
    std::string error_;
    std::array<std::unique_ptr<Card>,2> cards_;
    std::array<Handle,3> handles_{};
    std::vector<ListedEntry> listing_;
    std::size_t listing_next_=0;
    bool listing_valid_=false;
};
}
