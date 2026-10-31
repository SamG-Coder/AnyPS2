#include <nt/packed_image.hpp>
#include <nt/iso9660.hpp>
#include <iostream>
#include <stdexcept>
int main(int argc,char** argv) {
    if (argc!=3 && argc!=5) {
        std::cerr<<"Usage: ps2-unpack <packed-image> <new-output.elf>\n"
            "       ps2-unpack --disc <disc.iso> <disc-path> <new-output.elf>\n"; return 2;
    }
    try {
        nt::Bytes input;
        const std::filesystem::path output=argv[argc-1];
        if (std::filesystem::exists(output)) throw std::runtime_error("Output already exists");
        if (argc==5) {
            if (std::string_view(argv[1])!="--disc") throw std::runtime_error("Expected --disc");
            nt::disc::Iso9660 disc(argv[2]); input=disc.read(disc.find(argv[3]),32U*1024*1024);
        } else input=nt::read_file(argv[1],32U*1024*1024);
        const auto image=nt::package::unpack(input);
        nt::write_file(output,nt::package::elf_image(image));
        std::cout<<"Unpacked private conversion input; entry="<<nt::hex32(image.entry)
            <<" sections="<<image.segments.size()<<". Code is not compiled or executed.\n";
        return 0;
    } catch (const std::exception& error) { std::cerr<<"ps2-unpack: "<<error.what()<<'\n'; return 1; }
}
