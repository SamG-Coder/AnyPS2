#include <nt/ee_codegen.hpp>
#include "disc_fixture.hpp"
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void text(const std::filesystem::path& path,const std::string& value) {
    nt::write_file(path,{reinterpret_cast<const std::uint8_t*>(value.data()),value.size()});
}
std::string load(const std::filesystem::path& path) {
    const auto bytes=nt::read_file(path); return {bytes.begin(),bytes.end()};
}
std::string field(std::string value,const std::string& key,const std::string& replacement) {
    const auto begin=value.find(key+'=');
    if (begin==std::string::npos) throw std::runtime_error("Missing fixture field: "+key);
    const auto end=value.find('\n',begin);
    value.replace(begin,end-begin,key+'='+replacement); return value;
}
}
int main(int argc,char** argv) {
    if (argc!=4) return 1;
    try {
        const std::filesystem::path input=argv[1],capture=argv[2],root=argv[3];
        if (!std::filesystem::create_directory(root)) throw std::runtime_error("Fixture output already exists");
        const auto manifest=load(capture/"capture.txt"),cpu=load(capture/"cpu.txt");
        const auto ram=nt::read_file(capture/"ram.bin"),elf=nt::read_file(input);
        const auto scratch=root/"capture"; std::filesystem::create_directory(scratch);
        const auto source=root/"input.elf";
        const std::array observations{scratch};
        const auto reset=[&] {
            text(scratch/"capture.txt",manifest); text(scratch/"cpu.txt",cpu);
            nt::write_file(scratch/"ram.bin",ram); nt::write_file(source,elf);
        };
        unsigned count=0;
        const auto reject=[&](const char* expected,bool generate=false) {
            const auto output=root/("rejected-"+std::to_string(++count));
            bool rejected=false;
            try {
                if (generate) nt::ee_codegen::project(source,output,{}, {},observations);
                else (void)nt::ee_codegen::verify_flow(source,output,{}, {},observations);
            } catch (const std::exception& error) {
                if (std::string(error.what()).find(expected)==std::string::npos) throw;
                rejected=true;
            }
            if (!rejected || std::filesystem::exists(output)) throw std::runtime_error("Invalid observation published output");
            reset();
        };
        reset();
        text(scratch/"capture.txt",field(manifest,"complete","false")); reject("complete v2");
        text(scratch/"capture.txt",field(manifest,"format","anyps2-stop-observation-v1")); reject("complete v2");
        text(scratch/"capture.txt",manifest+"complete=true\n"); reject("duplicate");
        text(scratch/"capture.txt",field(manifest,"input_identity","1")); reject("identity differs");
        text(scratch/"capture.txt",field(manifest,"active_image","2")); reject("unknown executable image");
        text(scratch/"cpu.txt",field(cpu,"stop","1")); reject("indirect transfer");
        text(scratch/"cpu.txt",field(cpu,"delay_slot","1")); reject("indirect transfer");
        text(scratch/"cpu.txt",field(cpu,"pending_target","0")); reject("indirect transfer");
        text(scratch/"cpu.txt",field(cpu,"gpr31","0,0")); reject("supported indirect call");
        text(scratch/"cpu.txt",field(cpu,"branch_pc","4096")); reject("supported indirect call");
        text(scratch/"cpu.txt",field(cpu,"branch_pc","4608")); reject("not reached compiled flow");
        for (const auto target:{0x1200U,0x1240U,0x2000U,0x2010U}) {
            // VU section, executable BSS, data and data BSS are never EE roots.
            text(scratch/"cpu.txt",field(field(cpu,"pc",std::to_string(target)),"pending_target",std::to_string(target)));
            reject("outside the active file-backed EE code image");
        }
        nt::write_file(scratch/"ram.bin",nt::ByteView(ram).first(ram.size()-1)); reject("indirect transfer");
        for (const auto address:{0x1010U,0x1014U,0x1100U,0x111cU}) {
            auto changed=ram; changed[address]^=1; nt::write_file(scratch/"ram.bin",changed);
            reject("instruction bytes differ");
        }
        auto changed=elf; changed[0x200]^=1; nt::write_file(source,changed); reject("identity differs");
        // Unsupported code witnessed at a real call remains an explicit error.
        // This is a deliberately mutated synthetic input/capture, not game data.
        changed=elf; fixture::put32(changed,0x200,0x4c000000); nt::write_file(source,changed);
        (void)nt::ee_codegen::verify_flow(source,root/"unsupported-input");
        const auto identity=load(root/"unsupported-input"/"input-identity.txt");
        const auto begin=identity.find("input_identity=")+15;
        text(scratch/"capture.txt",field(manifest,"input_identity",identity.substr(begin,identity.find('\n',begin)-begin)));
        auto changed_ram=ram; fixture::put32(changed_ram,0x1100,0x4c000000); nt::write_file(scratch/"ram.bin",changed_ram);
        reject("unsupported operation families",true);
        std::cout<<"Rejected "<<count<<" invalid/unsupported observation cases without publishing output\n";
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
