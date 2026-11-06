#include <nt/iop_native_memory_card_files.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Files=nt::iop::NativeMemoryCardFiles;
void check(bool value,const char* detail) { if (!value) throw std::runtime_error(detail); }
std::span<const std::uint8_t> bytes(const std::string& text) {
    return {reinterpret_cast<const std::uint8_t*>(text.data()),text.size()};
}
std::string name_of(const Files::ListedEntry& entry) { return reinterpret_cast<const char*>(entry.data()+32); }
std::vector<std::string> names(Files& files,const std::string& pattern) {
    std::vector<Files::ListedEntry> listed;
    const auto count=files.list(0,0,pattern,0,64,listed);
    check(count>=0 && static_cast<std::size_t>(count)==listed.size(),"Listing count did not match its entries");
    std::vector<std::string> result;
    for (const auto& entry:listed) result.push_back(name_of(entry));
    return result;
}
std::string read_all(Files& files,const std::string& path) {
    const auto fd=files.open(0,0,path,Files::readable);
    check(fd>=0,"Could not open a stored file");
    std::vector<std::uint8_t> data(4096);
    const auto count=files.read(fd,data);
    check(count>=0,"Could not read a stored file");
    check(files.close(fd)==0,"Could not close a stored file");
    return std::string(data.begin(),data.begin()+count);
}
constexpr std::uint32_t create=Files::create_file|Files::writeable|Files::readable;

void card_identity(const std::filesystem::path& root) {
    Files none;
    check(none.detect(0,0,true)==Files::no_card && none.card_type(0,0)==0,"No folder still presented a card");
    Files files; files.set_root(root);
    check(files.detect(0,1,true)==Files::no_card && files.detect(2,0,true)==Files::no_card,"A missing port or multitap slot presented a card");
    check(files.detect(0,0,true)==Files::changed_card,"A new card was not reported once");
    check(files.detect(0,0,true)==Files::succeed && files.card_type(0,0)==2 && files.format_state(0,0)==1,"A card was not a formatted PS2 card");
    check(files.free_clusters(0,0)==Files::allocatable_clusters-1,"An empty card's only used cluster is its root directory");
}
void persistence(const std::filesystem::path& root) {
    std::vector<Files::ListedEntry> listed;
    {
        Files files; files.set_root(root);
        check(files.open(0,0,"/SAVE",Files::create_directory)==0,"Could not create a folder");
        check(files.open(0,0,"/SAVE",Files::create_directory)==Files::no_entry,"An existing folder was created again");
        std::string current;
        check(files.change_directory(0,0,"SAVE",current)==0 && current=="/SAVE","Change of folder did not report the path");
        const auto upper=files.open(0,0,"Data",create);
        check(upper>=0,"Could not create a file");
        check(files.write(upper,bytes("upper"))==5 && files.close(upper)==0,"Could not write a file");
        // These differ only by case, which a card allows and a Windows folder does not.
        const auto lower=files.open(0,0,"data",create);
        check(lower>=0 && files.write(lower,bytes("lower"))==5 && files.close(lower)==0,"Names differing by case collided");
        std::array<std::uint8_t,Files::entry_bytes> info{};
        info[8]=7; info[20]=1|2;
        check(files.set_info(0,0,"Data",info,Files::writeable|Files::executable)==0,"Could not set file information");
        check(read_all(files,"Data")=="upper" && read_all(files,"data")=="lower","Case-sensitive files were mixed up");
    }
    Files files; files.set_root(root);
    check(read_all(files,"/SAVE/Data")=="upper" && read_all(files,"/SAVE/data")=="lower","A save did not survive a restart");
    check(files.list(0,0,"/SAVE/Data",0,1,listed)==1 && listed[0][8]==7,"File information did not survive a restart");
    std::uint16_t mode; std::memcpy(&mode,listed[0].data()+20,2);
    check((mode&0x180f)==3 && (mode&Files::attribute_file) && (mode&Files::exists),"File attributes did not survive a restart");
    check(names(files,"/*")==std::vector<std::string>{"SAVE"},"The root listed entries it should not");
    check(names(files,"/SAVE/*")==(std::vector<std::string>{".","..","Data","data"}),"A folder did not list its entries");
    check(names(files,"/SAVE/?ata")==(std::vector<std::string>{"Data","data"}),"A one-character wildcard did not match");
    check(names(files,"/SAVE/nothing*").empty(),"A wildcard matched nothing it should");
    check(files.list(0,0,"/MISSING/*",0,8,listed)==Files::no_entry,"A missing folder was listed");
    listed.clear();
    check(files.list(0,0,"/SAVE/*",0,2,listed)==2 && files.list(0,0,"/SAVE/*",1,8,listed)==2 && listed.size()==4,"A listing did not continue");
    check(files.entry_space(0,0,"/SAVE")==0,"Two files and two links fill two directory clusters");
    check(files.remove(0,0,"/SAVE",0)==Files::not_empty,"A folder with files was deleted");
    check(files.remove(0,0,"/SAVE/Data",0)==0 && files.remove(0,0,"/SAVE/Data",0)==Files::no_entry,"Delete did not remove a file once");
    check(files.entry_space(0,0,"/SAVE")==1,"An odd entry count leaves one free slot");
    check(files.remove(0,0,"/SAVE/data",1)==Files::no_entry,"A delete flag restored or removed a file");
}
// Crash safety: without close or flush the previous contents stay on the host.
void commit_points(const std::filesystem::path& root) {
    {
        Files files; files.set_root(root);
        const auto fd=files.open(0,0,"/commit",create);
        check(fd>=0 && files.write(fd,bytes("first"))==5 && files.close(fd)==0,"Could not store a first version");
    }
    {
        Files files; files.set_root(root);
        const auto fd=files.open(0,0,"/commit",create);
        check(fd>=0 && files.write(fd,bytes("second version"))==14,"Could not start a second version");
        // The object goes away with the file open, as a crash would leave it.
    }
    Files files; files.set_root(root);
    check(read_all(files,"/commit")=="first","An uncommitted write replaced the previous version");
    const auto fd=files.open(0,0,"/commit",Files::readable|Files::writeable);
    check(fd>=0 && files.seek(fd,-2,2)==3 && files.write(fd,bytes("ST"))==2 && files.seek(fd,-100,1)==0,"Seek did not follow the card's rules");
    check(files.flush(fd)==0 && files.close(fd)==0,"Flush did not commit a file");
    check(read_all(files,"/commit")=="firST","An overwrite in place changed the wrong bytes");
}
void limits(const std::filesystem::path& root) {
    Files files; files.set_root(root);
    check(files.open(0,0,"/absent",Files::readable)==Files::no_entry,"A missing file opened");
    check(files.open(0,0,"/no/such/file",create)==Files::no_entry,"A file was created in a missing folder");
    check(files.open(0,0,"",Files::readable)==Files::no_entry,"An empty name opened");
    check(files.open(0,0,"/"+std::string(32,'x'),create)==Files::no_entry,"An overlong name was created");
    check(files.read(9,{})==Files::denied && files.close(-1)==Files::denied,"An invalid descriptor was accepted");
    const auto first=files.open(0,0,"/h1",create),second=files.open(0,0,"/h2",create),third=files.open(0,0,"/h3",create);
    check(first>=0 && second>=0 && third>=0,"Three handles should be available");
    check(files.open(0,0,"/h4",create)==Files::handle_limit,"A fourth handle was granted");
    check(files.open(0,0,"/h1",Files::writeable)==Files::handle_limit,"Handle limit did not apply");
    check(files.close(second)==0,"Could not free a handle");
    check(files.open(0,0,"/h1",Files::writeable)==Files::denied,"Two writers shared a file");
    check(files.write(first,{})==0 && files.remove(0,0,"/h1",0)==Files::denied,"An open file was deleted");
    std::vector<std::uint8_t> buffer(16);
    const auto reader=files.open(0,0,"/h2",Files::readable);
    check(reader>=0 && files.write(reader,bytes("x"))==Files::denied,"A read handle was written");
    check(files.close(reader)==0 && files.close(first)==0 && files.close(third)==0,"Could not close handles");
    check(files.read(reader,buffer)==Files::denied,"A closed handle stayed open");
    // Writing past the end of the card stores what fits, then reports a full card.
    const auto large=files.open(0,0,"/large",create);
    const auto capacity=std::uint64_t(files.free_clusters(0,0))*Files::cluster_bytes;
    const std::vector<std::uint8_t> filler(capacity+2048,0x5a);
    check(large>=0 && files.write(large,filler)==static_cast<int>(capacity),"A full card did not take what fits");
    check(files.write(large,bytes("x"))==Files::full_device,"A full card accepted more data");
    check(files.free_clusters(0,0)==0 && files.close(large)==0,"A full card reported free space");
    check(files.open(0,0,"/more",Files::create_directory)==Files::full_device,"A full card accepted a folder");
    check(files.remove(0,0,"/large",0)==0 && files.free_clusters(0,0)>0,"Deleting did not free space");
}
// Hostile names and manifests must stay inside the card's folder.
void containment(const std::filesystem::path& base) {
    const auto root=base/"contain";
    const auto outside=root/"outside.txt";
    std::filesystem::create_directories(root/"slot1");
    { std::ofstream(outside)<<"keep"; }
    {
        Files files; files.set_root(root);
        check(files.open(0,0,"/../outside.txt",Files::readable)==Files::no_entry,"A path escaped the card");
        check(files.open(0,0,"../../outside.txt",Files::writeable)==Files::no_entry,"A relative path escaped the card");
        for (const std::string name:{"a:b","c\\d","CON","nul.txt","...","tail."," space"}) {
            const auto fd=files.open(0,0,"/"+name,create);
            check(fd>=0 && files.write(fd,bytes(name))==static_cast<int>(name.size()) && files.close(fd)==0,"A card name could not be stored");
        }
    }
    {
        Files files; files.set_root(root);
        for (const std::string name:{"a:b","c\\d","CON","nul.txt","...","tail."," space"})
            check(read_all(files,"/"+name)==name,"A card name did not survive its host name");
    }
    check(std::filesystem::exists(outside),"A card operation touched a file outside its folder");
    const auto manifest=root/"slot2"/".anyps2-card";
    std::filesystem::create_directories(root/"slot2");
    { std::ofstream(manifest)<<"6162 ..\\..\\outside.txt 33815 0 0 0000000000000000 0000000000000000\n"; }
    Files files; files.set_root(root);
    check(files.detect(1,0,true)==Files::no_card && !files.error().empty(),"A manifest naming another folder was accepted");
}
void hand_copied(const std::filesystem::path& base) {
    const auto root=base/"copied";
    std::filesystem::create_directories(root/"slot1"/"FOLDER");
    { std::ofstream(root/"slot1"/"FOLDER"/"file.bin",std::ios::binary)<<"copied"; }
    Files files; files.set_root(root);
    check(read_all(files,"/FOLDER/file.bin")=="copied","A file copied into a card was not found");
    check(names(files,"/FOLDER/*")==(std::vector<std::string>{".","..","file.bin"}),"A copied file was not listed");
}
void formatting(const std::filesystem::path& base) {
    const auto root=base/"format";
    Files files; files.set_root(root);
    const auto fd=files.open(0,0,"/kept",create);
    check(fd>=0 && files.write(fd,bytes("save"))==4 && files.close(fd)==0,"Could not store a file to format away");
    const auto open=files.open(0,0,"/kept",Files::readable);
    check(open>=0 && files.format(0,0)==0,"Format failed");
    check(files.close(open)==Files::denied,"Format left a handle open");
    check(names(files,"/*").empty(),"Format left entries on the card");
    check(std::filesystem::exists(root/"slot1.before-format-1"/"kept"),"Format destroyed earlier saves");
}
}

int main() {
    const auto base=std::filesystem::temp_directory_path()/"anyps2-memory-card-test";
    std::error_code ignored;
    std::filesystem::remove_all(base,ignored);
    try {
        card_identity(base/"identity");
        persistence(base/"persist");
        commit_points(base/"commit");
        limits(base/"limits");
        containment(base);
        hand_copied(base);
        formatting(base);
    } catch (const std::exception& error) {
        std::cerr<<error.what()<<'\n';
        std::filesystem::remove_all(base,ignored);
        return 1;
    }
    std::filesystem::remove_all(base,ignored);
    std::cout<<"memory card files passed\n";
    return 0;
}
