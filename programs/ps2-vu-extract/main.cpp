#include <nt/vu_upload.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>

int main(int argc,char** argv) {
    if (argc!=4) {
        std::cerr<<"Usage: ps2-vu-extract <packet.bin> <new-private-directory> <unit:0|1>\n";
        return 2;
    }
    try {
        const std::string_view unit_text=argv[3];
        if (unit_text!="0" && unit_text!="1") throw std::invalid_argument("Expected VU unit 0 or 1");
        const auto sequence=nt::vu::extract_uploads(nt::read_file(argv[1],0x100000),unit_text=="1" ? 1 : 0);
        const auto regions=nt::vu::assemble_uploads(sequence.uploads,unit_text=="1" ? 1 : 0);
        const std::filesystem::path directory=argv[2];
        if (!std::filesystem::create_directory(directory)) throw std::invalid_argument("Output directory already exists");
        std::ofstream manifest(directory/"uploads.txt");
        manifest<<"unit="<<unit_text<<"\nconsumed="<<sequence.consumed<<'\n';
        if (sequence.stopped_command) manifest<<"stopped_command="<<*sequence.stopped_command<<'\n';
        unsigned index=0;
        for (const auto& upload:sequence.uploads) {
            const auto filename="upload-"+std::to_string(index++)+".bin";
            nt::write_file(directory/filename,upload.bytes);
            manifest<<filename<<" destination="<<upload.destination<<" pairs="<<upload.bytes.size()/8<<'\n';
        }
        index=0;
        for (const auto& region:regions) {
            const auto filename="image-"+std::to_string(index++)+".bin";
            nt::write_file(directory/filename,region.bytes);
            manifest<<filename<<" destination="<<region.destination<<" pairs="<<region.bytes.size()/8<<'\n';
        }
        manifest.flush();
        if (!manifest) throw std::runtime_error("Cannot write upload manifest");
        std::cout<<"Extracted "<<sequence.uploads.size()<<" upload chunks; consumed "<<sequence.consumed<<" bytes";
        if (sequence.stopped_command) std::cout<<"; stopped before command "<<*sequence.stopped_command;
        std::cout<<". No uploaded code was executed.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr<<"VU extraction: "<<error.what()<<'\n'; return 1; }
}
