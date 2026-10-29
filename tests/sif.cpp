#include <nt/system_bus.hpp>
#include <nt/kernel.hpp>
#include <iostream>

namespace {
using nt::hardware::SystemBus;
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class F> void rejects(F operation) {
    bool fault = false;
    try { operation(); } catch (const nt::runtime::MemoryFault&) { fault = true; }
    check(fault,"Unsupported SIF operation accepted");
}
void registers() {
    SystemBus bus;
    check(bus.load(0x1000c000,4) == 0 && bus.load(0x1000c400,4) == 0,"DMA reset state incorrect");
    check(bus.load(0x1000f520,4) == 0x1201,"DMAC enable reset incorrect");
    bus.store(0x1000f200,4,0x1234);
    check(bus.sif().iop_read_register(0) == 0x1234,"EE command address not visible to IOP");
    bus.sif().iop_write_register(1,0x5678);
    check(bus.load(0xb000f210,4) == 0x5678,"IOP command address/alias failed");
    bus.store(0x1000f220,4,5); bus.store(0x1000f220,4,2);
    bus.sif().iop_write_register(2,1);
    check(bus.load(0x1000f220,4) == 6,"MSFLAG set/clear ownership failed");
    bus.sif().iop_write_register(3,7); bus.store(0x1000f230,4,2);
    check(bus.sif().iop_read_register(3) == 5,"SMFLAG set/clear ownership failed");
    rejects([&] { bus.store(0x1000f210,4,1); });
    rejects([&] { bus.sif().iop_write_register(0,1); });
    rejects([&] { bus.load(0x1000c000,8); });
    rejects([&] { bus.load(0x1000c004,4); });
    rejects([&] { bus.store(0x1000f240,4,0x80000); });
    rejects([&] { bus.store(0x1000c000,4,0x109); });
    check(bus.load(0x1000c000,4) == 0,"Rejected start mutated channel");
    bus.store(0x1000c020,4,0x12345);
    check(bus.load(0x1000c020,4) == 0x2345,"QWC not truncated to 16 bits");
}
void inbound() {
    SystemBus bus;
    std::vector<std::uint8_t> payload(4096);
    for (unsigned i = 0; i < payload.size(); ++i) payload[i] = static_cast<std::uint8_t>(i * 17);
    bus.sif().iop_install(0x1000,payload);
    bus.sif().iop_send(0x1000,0x4000,256);
    rejects([&] { bus.sif().iop_send(0x1000,0x4000,1); });
    bus.advance_cycles(256); // Producer stalls at the bounded FIFO before EE starts.
    check(bus.sif().iop_send_pending() && bus.load(0x4000,4) == 0,"Missing FIFO backpressure");
    bus.store(0x1000c000,4,0x104);
    bus.advance_cycles(4);
    check(bus.load(0x4000,4) == 0 && (bus.load(0x1000c000,4) & 0x100),"DMA ran while globally disabled");
    bus.store(0x1000e000,4,1);
    bus.store(0x1000e010,4,1U << 21);
    bus.advance_cycles(600);
    for (unsigned i = 0; i < payload.size(); ++i)
        check(bus.load(0x4000+i,1) == payload[i],"SIF0 corrupted payload");
    check(bus.load(0x1000c010,4) == 0x5000 && bus.load(0x1000c020,4) == 0 && !(bus.load(0x1000c000,4)&0x100),"SIF0 did not complete");
    check(bus.interrupt_lines() == 0x800 && bus.interrupt_pending(),"DMAC completion not on IP3");
    bus.store(0x1000e010,4,1U << 5);
    check(!bus.interrupt_pending(),"DMA completion acknowledgement failed");
    bus.store(0x1000c000,4,0x104); bus.advance_cycles(10);
    check((bus.load(0x1000c000,4)&0x100) && !(bus.load(0x1000e010,4)&32),"Empty destination chain falsely completed");
    rejects([&] { bus.store(0x1000c010,4,0x1000); });
    bus.store(0x1000c000,4,0);
    rejects([&] { bus.sif().iop_send(0x1ffff0,0,2); });
}
void outbound(bool chained) {
    SystemBus bus;
    // Original synthetic packet: IOP destination tag followed by two quadwords.
    bus.store_quad(0x2000,{0x800000100ULL | 0x80000000ULL,0}); // 8 words, destination 0x100.
    bus.store_quad(0x2010,{0x1122334455667788ULL,0x99aabbccddeeff00ULL});
    bus.store_quad(0x2020,{1,2});
    if (chained) {
        bus.store_quad(0x1000,{(std::uint64_t(0x2000) << 32) | 3,0}); // REFE.
        bus.store(0x1000c430,4,0x1000);
    } else {
        bus.store(0x1000c410,4,0x2000); bus.store(0x1000c420,4,3);
    }
    bus.store(0x1000e000,4,1);
    bus.store(0x1000f590,4,0x11201); // Suspend DMA before starting.
    bus.store(0x1000c400,4,chained ? 0x105 : 0x101);
    bus.advance_cycles(8);
    check(bus.sif().iop_received_packets() == 0,"DMA suspend ignored");
    bus.store(0x1000f590,4,0x1201); bus.advance_cycles(20);
    const auto memory = bus.sif().iop_memory();
    for (unsigned i = 0; i < 32; ++i) check(memory[0x100+i] == bus.load(0x2010+i,1),"SIF1 payload mismatch");
    check(bus.sif().iop_received_packets() == 1 && !(bus.load(0x1000c400,4)&0x100) && (bus.load(0x1000e010,4)&64),"SIF1 completion failed");
    check(bus.load(0x1000f230,4) == 0,"Transport invented IOP boot/RPC readiness");
}
void invalid_chain() {
    SystemBus bus;
    bus.store_quad(0x1000,{0x50000001,0}); // CALL requires an unsupported chain stack.
    bus.store(0x1000c430,4,0x1000); bus.store(0x1000c400,4,0x105);
    bus.store(0x1000e000,4,1);
    rejects([&] { bus.advance_cycles(1); });
    check(bus.load(0x1000c430,4) == 0x1000 && !(bus.load(0x1000e010,4)&64),"Rejected tag mutated progress");
}
void kernel_services() {
    SystemBus bus;
    nt::kernel::Kernel kernel;
    nt::runtime::State state;
    const auto call = [&](unsigned number, unsigned id = 0, unsigned value = 0) {
        state.set(3,number); state.set(4,id); state.set(5,value);
        return kernel.service(state,bus);
    };
    check(call(0x78) && bus.load(0x1000c000,4) == 0x184 && bus.load(0x1000e000,4) == 1,"SIF receive chain setup failed");
    check(call(0x7a,2) && state.gpr[2].lo == 0x1000 && bus.load(0x1000f230,4) == 0x30000,"IOP command service did not publish readiness");
    check(!call(0x78),"SIF rearm discarded an active receive");
    bus.advance_cycles(20);
    check(bus.load(0x1000c000,4) == 0x184,"Empty HLE receive chain completed");
    for (unsigned id = 0x80000000; id < 0x80000003; ++id) {
        check(call(0x7a,id) && state.gpr[2].lo == 0,"Software SIF register not reset");
        check(call(0x79,id,0xf1234567) && call(0x7a,id) && state.gpr[2].lo == 0xfffffffff1234567ULL,"Software SIF register round trip failed");
    }
    check(call(0x79,1,0x1230) && bus.sif().iop_read_register(0) == 0x1230,"SIF kernel register write disconnected from hardware");
    check(!call(0x7a,0x80000003) && !call(0x79,0,1),"Unknown SIF register accepted");
}
void command_service() {
    SystemBus bus;
    bus.initialize_sif(); bus.store(0x1000e000,4,1);
    const auto send = [&](unsigned command, unsigned length, unsigned argument, unsigned value = 0, unsigned option = 0) {
        bus.store_quad(0x2000,{(std::uint64_t(8) << 32) | 0x80001000,0});
        bus.store_quad(0x2010,{length,std::uint64_t(command) | (std::uint64_t(option) << 32)});
        bus.store_quad(0x2020,{std::uint64_t(argument) | (std::uint64_t(value) << 32),0});
        bus.store(0x1000c410,4,0x2000); bus.store(0x1000c420,4,3); bus.store(0x1000c400,4,0x101);
        bus.advance_cycles(10);
    };
    send(0x80000002,20,0x4000);
    check(bus.sif().ee_command_address() == 0x4000 && (bus.load(0x1000f220,4)&0x20000),"SIF INIT_CMD handshake failed");
    send(0x80000000,20,0x5000);
    check(bus.sif().ee_command_address() == 0x5000,"SIF address change failed");
    send(0x80000001,24,7,0xfeed);
    check(bus.sif().command_register(7) == 0xfeed,"SIF software register update failed");
    bus.store(0x1000c000,4,0x184);
    send(0x80000002,16,0,0,1); bus.advance_cycles(10);
    check(bus.sif().ee_command_address() == 0x5000 && bus.load(0x5000,4) == 24 &&
          bus.load(0x5008,4) == 0x80000001 && bus.load(0x5010,4) == 0 && bus.load(0x5014,4) == 1,
          "RPC initialization did not send the software-register response via SIF0");
    bus.store(0x1000c000,4,0x184);
    const std::array<std::uint32_t,12> bind{36,0,0x80000009U,0,7,0x9000,9,0xa000,0x12345678,0,0,0};
    for (unsigned i = 0; i < bind.size(); ++i) bus.store(0x3000+i*4,4,bind[i]);
    const std::array<nt::runtime::SifTransfer,1> transfer{{{0x3000,0x1000,48,0x44}}};
    check(bus.submit_sif(transfer) != 0,"RPC bind submission failed");
    bus.advance_cycles(30);
    check(bus.load(0x5000,4) == 48 && bus.load(0x5008,4) == 0x80000008 &&
          bus.load(0x5010,4) == 7 && bus.load(0x5014,4) == 0x9000 && bus.load(0x5018,4) == 9 &&
          bus.load(0x501c,4) == 0xa000 && bus.load(0x5020,4) == 0x80000009 && bus.load(0x5024,4) == 0,
          "Unregistered RPC server did not receive a correlated null bind response");
    check(bus.sif().rpc_bind_requests() == 1 && bus.sif().last_rpc_bind_server() == 0x12345678,
          "RPC bind diagnostic lost the requested server");
    rejects([&] { send(0x80000002,20,0x6000,0,2); });
    check(bus.sif().ee_command_address() == 0x5000,"Unsupported init option mutated state");
}
void hle_jobs() {
    SystemBus bus;
    nt::kernel::Kernel kernel;
    nt::runtime::State state;
    bus.store(0x1000e000,4,1);
    bus.store_quad(0x2000,{0x123456789abcdef0ULL,0xfedcba9876543210ULL});
    bus.store_quad(0x2010,{11,22});
    const std::array<nt::runtime::SifTransfer,2> transfers{{{0x2000,0x300,16,0},{0x2010,0x320,1,0x44}}};
    for (unsigned i = 0; i < transfers.size(); ++i) {
        const auto base = 0x4000 + i * 16;
        bus.store(base,4,transfers[i].source); bus.store(base+4,4,transfers[i].destination);
        bus.store(base+8,4,transfers[i].bytes); bus.store(base+12,4,transfers[i].attributes);
    }
    state.set(3,0x77); state.set(4,0x4000); state.set(5,2);
    check(kernel.service(state,bus) && state.gpr[2].lo == 1,"SifSetDma did not queue descriptors");
    check(bus.submit_sif(transfers) == 0,"Busy DMA queue accepted a second batch");
    rejects([&] { bus.store(0x1000c400,4,0x101); });
    state.set(3,0x76); state.set(4,1);
    check(kernel.service(state,bus) && state.gpr[2].lo == 1,"Pending DMA status reported completion");
    bus.advance_cycles(20);
    check(kernel.service(state,bus) && state.gpr[2].lo == ~std::uint64_t(0),"Completed DMA status failed");
    for (unsigned i = 0; i < 16; ++i) {
        check(bus.sif().iop_memory()[0x300+i] == bus.load(0x2000+i,1),"First HLE DMA payload corrupted");
        check(bus.sif().iop_memory()[0x320+i] == bus.load(0x2010+i,1),"Rounded HLE DMA payload corrupted");
    }
    auto invalid = transfers;
    invalid[1].destination = 0x1ffffc;
    rejects([&] { bus.submit_sif(invalid); });
    check(bus.sif_status(2) == -1 && bus.sif().iop_received_packets() == 2,"Invalid batch partially submitted");
    check(bus.submit_sif(transfers) == 2,"Rejected batch consumed an ID");
}
void source_chain() {
    SystemBus bus;
    bus.store_quad(0x1000,{0x10000002,0}); // CNT, inline IOP tag + first payload.
    bus.store_quad(0x1010,{(std::uint64_t(8)<<32)|0x80000300,0});
    bus.store_quad(0x1020,{1,2});
    bus.store_quad(0x1030,{(std::uint64_t(0x2000)<<32)|0x20000000,0}); // NEXT.
    bus.store_quad(0x2000,{(std::uint64_t(0x3000)<<32)|0x30000001,0}); // REF.
    bus.store_quad(0x2010,{0x70000000,0}); // END.
    bus.store_quad(0x3000,{3,4});
    bus.store(0x1000c430,4,0x1000); bus.store(0x1000c400,4,0x105); bus.store(0x1000e000,4,1);
    bus.advance_cycles(30);
    check(bus.sif().iop_received_packets() == 1 && bus.sif().iop_memory()[0x300] == 1 &&
          bus.sif().iop_memory()[0x310] == 3 && !(bus.load(0x1000c400,4)&0x100),"CNT/NEXT/REF/END chain failed");
    // TTE sends the upper tag words before payload; two final padding words
    // deliberately remain in the FIFO and cannot become a complete IOP tag.
    SystemBus tte;
    tte.store_quad(0x1000,{(std::uint64_t(0x2000)<<32)|2,(std::uint64_t(4)<<32)|0x80000400});
    tte.store_quad(0x2000,{0,0x1122334455667788ULL});
    tte.store_quad(0x2010,{0x99aabbccddeeff00ULL,0});
    tte.store(0x1000c430,4,0x1000); tte.store(0x1000c400,4,0x145); tte.store(0x1000e000,4,1);
    tte.advance_cycles(20);
    check(tte.sif().iop_received_packets() == 1 && tte.sif().iop_memory()[0x400] == 0x88 &&
          tte.sif().iop_memory()[0x408] == 0 && (tte.load(0x1000e010,4)&64),"Tag-transfer stream assembly failed");
}
void malformed_command() {
    for (const auto command : {0x80000001U,0x8000000aU,0x80000003U}) {
        SystemBus bus; bus.initialize_sif(); bus.store(0x1000e000,4,1);
        bus.store_quad(0x2000,{16,command});
        const std::array<nt::runtime::SifTransfer,1> transfer{{{0x2000,0x1000,16,0x44}}};
        const auto id = bus.submit_sif(transfer);
        rejects([&] { bus.advance_cycles(10); });
        check(bus.sif_status(id) == 1 && bus.sif().iop_received_packets() == 0,
              "Malformed command or unimplemented RPC/reset was acknowledged");
    }
}
}
int main() {
    try { registers(); inbound(); outbound(false); outbound(true); invalid_chain(); kernel_services(); command_service(); hle_jobs(); source_chain(); malformed_command();
        std::cout << "SIF register, DMA, FIFO and IOP transport tests passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
