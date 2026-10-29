#include "disc_fixture.hpp"
#include <nt/system_bus.hpp>
#include <chrono>
#include <iostream>

namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
nt::Bytes words(std::initializer_list<std::uint32_t> values) {
    nt::Bytes data(values.size()*4); unsigned at=0;
    for (auto value : values) { fixture::put32(data,at,value); at+=4; }
    return data;
}
void exercise(const std::filesystem::path& path) {
    nt::Bytes payload(5003);
    for (unsigned i=0;i<payload.size();++i) payload[i]=static_cast<std::uint8_t>(i*37);
    nt::Bytes iso(25*2048,0);
    const auto pvd=16*2048;
    iso[pvd]=1; const std::string signature="CD001";
    std::copy(signature.begin(),signature.end(),iso.begin()+pvd+1);
    iso[pvd+6]=1; iso[pvd+129]=8; iso[pvd+130]=8;
    fixture::record(iso,pvd+156,std::string(1,'\0'),20,2048,true);
    fixture::record(iso,20*2048,"DATA.BIN;1",21,static_cast<std::uint32_t>(payload.size()),false);
    std::copy(payload.begin(),payload.end(),iso.begin()+21*2048);
    nt::write_file(path,iso);
    nt::hardware::SystemBus bus;
    bus.sif().mount_disc(path); bus.initialize_sif(); bus.store(0x1000e000,4,1);
    const auto install = [&](std::uint32_t address, const nt::Bytes& data) {
        for (unsigned i=0;i<data.size();++i) bus.store(address+i,1,data[i]);
    };
    const auto send = [&](const nt::Bytes& packet, const nt::Bytes& args = {}) {
        install(0x6000,packet); install(0x70001000,args);
        std::vector<nt::runtime::SifTransfer> transfers;
        if (!args.empty()) transfers.push_back({0x70001000,0x2000,static_cast<std::uint32_t>(args.size()),0});
        transfers.push_back({0x6000,0x1000,static_cast<std::uint32_t>(packet.size()),0x44});
        const auto job=bus.submit_sif(transfers);
        check(job!=0,"RPC input queue busy");
        for (unsigned cycle=0;cycle<10000;++cycle) {
            bus.advance_cycles(1);
            if (nt::le32(packet,8)==0x8000000a && (bus.load(0x1000e010,4)&32))
                check(bus.load(0x4008,4)==0x80000008 && bus.load(0x4020,4)==0x8000000a,
                      "SIF0 interrupted before the RPC completion packet arrived");
        }
        check(bus.sif_status(job)==-1,"RPC input did not complete");
    };
    send(words({20,0,0x80000002,0,0x4000}));
    bus.store(0x1000c000,4,0x184); send(words({16,0,0x80000002,1}));
    check(bus.load(0x4014,4)==1,"RPC initialization failed");
    bus.store(0x1000c000,4,0x184); send(words({36,0,0x80000009,0,7,0x9000,9,0xa000,0x80000001}));
    const auto server=static_cast<std::uint32_t>(bus.load(0x4024,4));
    check(server && bus.load(0x4028,4)==0x2000,"Disc-backed FILEIO bind failed");
    const auto call = [&](unsigned method,const nt::Bytes& args,unsigned result_size=4) {
        bus.store(0x1000e010,4,32); bus.store(0x1000c000,4,0x184);
        bus.store(0x4008,4,0);
        send(words({56,0,0x8000000a,0,17,0x9000,19,0xa000,method,
                    static_cast<std::uint32_t>(args.size()),0x5004,result_size,1,server}),args);
        check(!(bus.load(0x1000c000,4)&0x100) && (bus.load(0x1000e010,4)&32),"RPC response did not finish SIF0");
        check(bus.load(0x4010,4)==17 && bus.load(0x4018,4)==19 && bus.load(0x4020,4)==0x8000000a,
              "RPC completion correlation lost");
        return static_cast<std::int32_t>(bus.load(0x5004,4));
    };
    bus.store_quad(0x5000,{0xccccccccccccccccULL,0xccccccccccccccccULL});
    call(255,{},8);
    check(bus.load(0x5004,4)==0x30303031 && bus.load(0x5008,4)==0x2e2e2e2e && bus.load(0x5000,4)==0xcccccccc && bus.load(0x500c,4)==0xcccccccc,
          "Version result or partial-quadword preservation failed");
    const auto open = [&](std::string name,unsigned mode=1) {
        auto args=words({mode}); args.insert(args.end(),name.begin(),name.end()); args.push_back(0);
        return call(0,args);
    };
    const auto fd=open("cdrom0:\\data.bin;1"); check(fd>=3,"Open failed");
    for (unsigned i=0;i<5024;++i) bus.store(0x8000+i,1,0xcc);
    check(call(2,words({static_cast<unsigned>(fd),0x8003,6000,0xa000}))==5003,"Read did not stop at EOF");
    for (unsigned i=0;i<payload.size();++i) check(bus.load(0x8003+i,1)==payload[i],"Disc payload corrupted");
    check(bus.load(0x8002,1)==0xcc && bus.load(0x8003+5003,1)==0xcc && bus.load(0xa000,8)==0,
          "Unaligned read damaged neighbours or callback fixup");
    check(call(2,words({static_cast<unsigned>(fd),0x8003,16,0}))==0,"EOF did not return zero");
    check(call(4,words({static_cast<unsigned>(fd),static_cast<unsigned>(-3),2}))==5000,"Seek from end failed");
    check(call(2,words({static_cast<unsigned>(fd),0x8003,3,0}))==3 && bus.load(0x8003,1)==payload[5000],"Seek/read failed");
    check(call(1,words({static_cast<unsigned>(fd)}))==0 && call(1,words({static_cast<unsigned>(fd)}))==-9,"Close or invalid handle failed");
    check(open("cdrom0:/missing")==-2 && open("cdrom0:/../DATA.BIN")==-2 && open("host:C:/file")==-19,
          "Missing file or guest path isolation failed");
    check(open("cdrom0:/DATA.BIN",2)==-30 && call(3,words({0,0,0}))==-30,"Read-only disc accepted writes");
    const auto second=open("cdrom:/DATA.BIN");
    bool rejected=false;
    try { call(2,words({static_cast<unsigned>(second),0x1fffff0,32,0})); }
    catch(const nt::runtime::MemoryFault&) { rejected=true; }
    check(rejected,"Out-of-range guest read destination accepted");
}
}
int main() {
    const auto path=std::filesystem::temp_directory_path()/
        ("anyps2-fileio-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".iso");
    try { exercise(path); std::filesystem::remove(path); std::cout<<"Disc-backed FILEIO RPC integration passed\n"; }
    catch(const std::exception& error) { std::error_code ignored; std::filesystem::remove(path,ignored); std::cerr<<error.what()<<'\n'; return 1; }
}
