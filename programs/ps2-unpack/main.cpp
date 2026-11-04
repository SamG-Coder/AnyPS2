#include <nt/packed_image.hpp>
#include <nt/iso9660.hpp>
#include <iostream>
#include <stdexcept>
int main(int argc,char** argv) {
    const bool from_disc=argc>1 && std::string_view(argv[1])=="--disc";
    const int required=from_disc ? 5 : 3;
    if (argc!=required && argc!=required+2) {
        std::cerr<<"Usage: ps2-unpack <packed-image> <new-output.elf> [--layout classification.txt]\n"
            "       ps2-unpack --disc <disc.iso> <disc-path> <new-output.elf> [--layout classification.txt]\n"; return 2;
    }
    try {
        nt::Bytes input;
        const std::filesystem::path output=argv[required-1];
        if (std::filesystem::exists(output)) throw std::runtime_error("Output already exists");
        if (from_disc) {
            nt::disc::Iso9660 disc(argv[2]); input=disc.read(disc.find(argv[3]),32U*1024*1024);
        } else input=nt::read_file(argv[1],32U*1024*1024);
        std::vector<nt::package::Region> layout;
        if (argc==required+2) {
            if (std::string_view(argv[required])!="--layout") throw std::runtime_error("Expected --layout");
            layout=nt::package::parse_layout(nt::read_file(argv[required+1],1024*1024),nt::fingerprint(input));
        }
        const auto image=nt::package::unpack(input);
        nt::write_file(output,nt::package::elf_image(image,layout));
        std::cout<<"Unpacked private conversion input; entry="<<nt::hex32(image.entry)
            <<" segments="<<image.segments.size()<<" classified_regions="<<layout.size()
            <<" input_identity="<<nt::fingerprint(input)<<". Code is not compiled or executed.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr<<"ps2-unpack: "<<error.what()<<'\n'; return 1; }
}
