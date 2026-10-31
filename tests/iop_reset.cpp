#include "../programs/ee-native-probe/iop_reset.hpp"
#include "disc_fixture.hpp"
#include <chrono>
#include <source_location>

namespace {
void require(bool condition,std::source_location at=std::source_location::current()) {
    if (!condition) throw std::runtime_error("Native reset lifecycle failed at "+std::to_string(at.line()));
}
nt::Bytes initial_data() {
    nt::Bytes bytes(0x600,0);
    for (unsigned index=0;index<2;++index) {
        fixture::put32(bytes,0x400+index*8,0x03e00008);
        fixture::put32(bytes,0x404+index*8,0x24020000|(11*(index+1)));
    }
    return bytes;
}
}
// Metadata for the original synthetic entries compiled by emit-iop-fixture.
nt::iop::NativeModule nt_iop_generated_module() { return {0xa100,0xa500,0x600,0x600,0}; }
std::uint64_t nt_iop_generated_data_hash() { return nt::fingerprint(initial_data()); }
std::vector<std::uint64_t> nt_iop_generated_source_hashes() { return {nt_iop_generated_data_hash()}; }
std::vector<nt::iop::SealedModule> nt_iop_generated_sealed() { return {}; }
std::vector<nt::iop::NativeModule> nt_iop_generated_entries() {
    return {{0xa100,0xa500,0x600,0x600,0},{0xa100,0xa508,0x600,0x600,0}};
}
std::vector<nt::iop::NativeLibrarySpec> nt_iop_generated_libraries() { return {}; }
std::vector<nt::iop::NativeImportSpec> nt_iop_generated_imports() { return {}; }

void run_iop_reset_tests() {
    using nt::ee::Stop;
    const auto directory=std::filesystem::temp_directory_path()/
        ("anyps2-reset-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(directory);
    const auto data=directory/"module.data",disc=directory/"fixture.iso";
    const auto bytes=initial_data();
    nt::Bytes archive(80+bytes.size(),0);
    const std::array<std::string,4> names{"RESET","ROMDIR","EXTINFO","EXAMPLE"};
    for (unsigned index=0;index<names.size();++index)
        std::copy(names[index].begin(),names[index].end(),archive.begin()+16*index);
    fixture::put32(archive,28,80); fixture::put32(archive,60,static_cast<std::uint32_t>(bytes.size()));
    std::copy(bytes.begin(),bytes.end(),archive.begin()+80);
    nt::write_file(data,bytes);
    nt::write_file(disc,fixture::single_file_iso("BOOT.IMG;1",archive));
    {
        nt::Bytes ram(0x10000,0);
        nt::ee::Memory memory{ram.data(),ram.size()};
        nt::ee::NativePlatform platform; platform.attach(memory);
        const auto protection=+[](std::uint32_t,std::uint32_t) { return false; };
        probe::IopReset reset(data,disc,memory,platform,protection,true);
        nt::ee::NativeSifCommandPeer peer(platform.sif_link,memory,protection,0x1000);
        require(peer.reset_owner(reset.owner()) && peer.connect(platform.sif_outbound));
        platform.sif_link.service(0x79,1,0x6000);
        platform.sif_link.service(0x79,3,0x10000);
        const auto send=[&](std::uint32_t destination,std::uint32_t command,std::string_view arguments={}) {
            std::fill(ram.begin()+0x4000,ram.begin()+0x4070,0);
            fixture::put32(ram,0x4000,104); fixture::put32(ram,0x4008,command);
            fixture::put32(ram,0x4010,static_cast<std::uint32_t>(arguments.size()+1));
            std::copy(arguments.begin(),arguments.end(),ram.begin()+0x4018);
            fixture::put32(ram,0x3000,0x4000); fixture::put32(ram,0x3004,destination);
            fixture::put32(ram,0x3008,104); fixture::put32(ram,0x300c,0x44);
            const auto submitted=platform.sif_outbound.submit(memory,0x3000,1);
            require(submitted.status==Stop::budget && submitted.value);
            return platform.sif_outbound.pump(memory);
        };
        constexpr std::string_view arguments="rom0:UDNL cdrom0:\\BOOT.IMG;1";
        require(send(0x1000,0x80000003,arguments)==Stop::budget && reset.generations()==1);
        require(!reset.finished() && !reset.returned() && !reset.executed() && !platform.sif_link.peer_flags());
        require(reset.pump()==Stop::budget && !reset.finished());
        platform.sif_link.service(0x79,4,0x10000);
        require(reset.pump()==Stop::budget && !reset.finished());
        platform.sif_link.service(0x79,4,0x20000);
        require(reset.pump()==Stop::budget && reset.finished() && reset.returned()==2 && reset.executed()==4);
        require(platform.sif_link.peer_address()==0x1f0000 && !reset.cdvd_initialized());
        // Leave an old command queued. Replacement must destroy it, not publish
        // its response into the next lifetime when that lifetime starts.
        require(send(0x1f0000,0x80000009)==Stop::budget);
        const auto before=ram;
        const auto owner=reset.owner();
        require(owner.reset(owner.context,{1,arguments}).status==Stop::unsupported_system && reset.generations()==1);
        auto damaged=bytes; damaged.front()=1; nt::write_file(data,damaged);
        bool rejected=false;
        try { owner.reset(owner.context,{0,arguments}); }
        catch (const std::runtime_error&) { rejected=true; }
        require(rejected && reset.generations()==1 && reset.finished() && platform.sif_link.peer_address()==0x1f0000);
        nt::write_file(data,bytes);
        require(send(0x1f0000,0x80000003,arguments)==Stop::budget && reset.generations()==2);
        require(!reset.finished() && !reset.returned() && reset.executed()==4 && !platform.sif_link.peer_flags());
        require(reset.pump()==Stop::budget && reset.executed()==4);
        platform.sif_link.service(0x79,4,0x30000);
        require(reset.pump()==Stop::budget && reset.finished() && reset.returned()==2 && reset.executed()==8);
        require(reset.last_server()==0 && !reset.cdvd_initialized());
        require(std::equal(before.begin()+0x6000,before.begin()+0x6070,ram.begin()+0x6000));
        require(reset.pump()==Stop::budget && reset.executed()==8);
    }
    std::filesystem::remove_all(directory);
}
