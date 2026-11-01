#include <nt/iop_native.hpp>
#include <nt/iop_native_serial.hpp>
#include <nt/iop_native_hard_timers.hpp>
#include <nt/iop_native_vblank.hpp>
#include <nt/iop_native_sound.hpp>
#include <nt/iop_native_usb.hpp>
#include <nt/iop_native_dev9.hpp>
#include <nt/iop_native_jump.hpp>
#include <nt/iop_native_heap.hpp>
#include <nt/iop_native_heaplib.hpp>
#include <nt/iop_native_libraries.hpp>
#include <nt/iop_native_boot.hpp>
#include <nt/iop_native_boot_callbacks.hpp>
#include <nt/iop_native_disc.hpp>
#include <nt/iop_native_drivers.hpp>
#include <nt/iop_native_sysclib.hpp>
#include <nt/iop_native_prnt.hpp>
#include <nt/iop_native_driver_files.hpp>
#include <nt/iop_native_interrupts.hpp>
#include <nt/iop_native_events.hpp>
#include <nt/iop_native_semaphores.hpp>
#include <nt/iop_native_fixed_pool.hpp>
#include <nt/iop_native_mailboxes.hpp>
#include <nt/iop_native_threads.hpp>
#include <nt/iop_native_system.hpp>
#include <nt/iop_native_alarms.hpp>
#include <nt/iop_native_sif.hpp>
#include <nt/iop_native_sif_control.hpp>
#include <nt/iop_native_sif_commands.hpp>
#include <nt/iop_native_sif_session.hpp>
#include <nt/iop_native_sif_inbound.hpp>
#include <nt/iop_native_rpc.hpp>
#include <nt/iop_native_rpc_data.hpp>
#include <nt/iop_native_stdio.hpp>
#include <nt/iop_native_modload.hpp>
#include <nt/iop_native_archive.hpp>
#include <nt/iop_devices.hpp>
#include <nt/iop_native_dmacman.hpp>
#include <nt/iop_native_cdvdman.hpp>
#include <nt/iop_native_secrman.hpp>
#include "reboot_fixture.hpp"
#include "disc_fixture.hpp"
#include <algorithm>
#include <chrono>
#include <string>
#include <vector>
#include <iostream>
#include <stdexcept>
#include <bit>

nt::iop::NativeResult nt_iop_generated_run(nt::iop::Cpu&,std::span<std::uint8_t>,std::uint64_t);
namespace {
void check(bool value,const char* detail) { if (!value) throw std::runtime_error(detail); }
nt::iop::Cpu initial(std::uint32_t alias=0) {
    nt::iop::Cpu cpu; cpu.pc=alias|0x1000; cpu.registers[2]=99;
    cpu.registers[4]=alias|0x2000; cpu.registers[31]=0x5000;
    return cpu;
}
// Original test service, not a claimed implementation of a PS2 syscall.
class FixtureServices final : public nt::iop::NativeServices {
public:
    unsigned calls=0;
    nt::iop::NativeServiceReply invoke(const nt::iop::NativeServiceRequest& request,std::span<std::uint8_t>) override {
        ++calls;
        if (request.selector==124) return {nt::iop::NativeServiceStatus::address_fault,0,0x300000};
        if (request.selector!=123) return {};
        return {nt::iop::NativeServiceStatus::returned,
            request.arguments[0]+request.arguments[1]+request.arguments[2]+request.arguments[3]};
    }
};
class FixtureAudio final : public nt::audio::Output {
public:
    bool allow_open=false,allow_initialize=true,allow_configure=true,opened=false;
    unsigned count=0,initializations=0;
    unsigned reverb_clears=0;
    std::array<nt::audio::VoiceMix,48> voice_mix{};
    std::array<nt::audio::CoreMix,2> core_mix{};
    std::array<nt::audio::StreamState,2> streams{};
    std::array<std::vector<std::int16_t>,2> pcm;
    bool submit_stream(unsigned core,std::span<const std::int16_t> data) override {
        if (core>=2 || !opened || streams[core].queued) return false;
        pcm[core].assign(data.begin(),data.end()); streams[core].queued=true; return true;
    }
    bool stream_state(unsigned core,nt::audio::StreamState& state) override { if (core>=2) return false; state=streams[core]; return true; }
    bool stop_stream(unsigned core) override { if (core>=2) return false; streams[core]={}; pcm[core].clear(); return true; }
    bool stop_voice(unsigned voice) override { return voice<48 && opened; }
    bool open() override { return opened=allow_open; }
    bool initialize(bool) override {
        if (!allow_initialize || !open()) return false;
        count=48; ++initializations; return true;
    }
    void close() override { opened=false; count=0; }
    bool configure_voice(unsigned index,const nt::audio::VoiceMix& mix) override {
        if (!count || !allow_configure || index>=voice_mix.size()) return false;
        voice_mix[index]=mix; return true;
    }
    bool configure_core(unsigned core,const nt::audio::CoreMix& mix) override {
        if (!count || !allow_configure || core>=core_mix.size()) return false;
        core_mix[core]=mix; return true;
    }
    bool clear_reverb(unsigned core) override { if (!allow_configure || core>=2) return false; ++reverb_clears; return true; }
    bool ready() const override { return opened; }
    unsigned voices() const override { return count; }
    std::uint32_t error() const override { return allow_open ? 0U : 1U; }
};
class FixtureUsb final : public nt::usb::Host {
public:
    bool available=false;
    bool allow_read=true,allow_input=true,input_complete=false;
    unsigned input_error=5;
    unsigned output_reports=0;
    unsigned cancelled=0;
    unsigned opened=0,closed=0,reads=0;
    std::vector<nt::usb::Device> devices{{L"synthetic-port",{18,1,0,2,0,0,0,64,0x34,0x12,0x78,0x56,0,1,0,0,0,1,
        9,2,25,0,1,1,0,0x80,50,9,4,0,0,1,3,0,0,0,7,5,0x81,3,8,0,1}}};
    bool scan(std::vector<nt::usb::Device>& result) override { if (!available) return false; result=devices; return true; }
    std::uint32_t error() const override { return !available ? 5U : allow_input ? 0U : input_error; }
    struct Input final : nt::usb::InputPipe {
        FixtureUsb& host; bool pending=false;
        explicit Input(FixtureUsb& value) : host(value) {}
        bool submit(std::uint32_t size) override { if (size<3 || pending) return false; pending=true; return true; }
        nt::usb::TransferState poll(std::vector<std::uint8_t>& bytes) override {
            if (!host.input_complete) return nt::usb::TransferState::pending;
            bytes={3,5,8}; pending=false; return nt::usb::TransferState::complete;
        }
        void cancel() override { if (pending) ++host.cancelled; pending=false; }
        std::uint32_t error() const override { return 122; }
    };
    std::unique_ptr<nt::usb::InputPipe> open_input(const nt::usb::Device&,const nt::usb::Endpoint& endpoint) override {
        if (!allow_input || endpoint.address!=0x81 || endpoint.interface_number) return {};
        return std::make_unique<Input>(*this);
    }
    bool location(const nt::usb::Device&,std::array<std::uint8_t,7>& path) override { path={2,3,0,0,0,0,0}; return true; }
    struct Control final : nt::usb::ControlPipe {
        FixtureUsb& host;
        explicit Control(FixtureUsb& value) : host(value) { ++host.opened; }
        ~Control() override { ++host.closed; }
        bool read_descriptor(std::uint16_t value,std::uint16_t index,std::uint16_t length,std::vector<std::uint8_t>& bytes) override {
            ++host.reads;
            if (!host.allow_read || value!=0x100 || index) return false;
            const auto& source=host.devices.front().descriptors;
            bytes.assign(source.begin(),source.begin()+std::min<unsigned>(length,18)); return true;
        }
        std::uint32_t error() const override { return host.allow_read ? 0 : 5; }
        bool use_configuration(std::uint8_t configuration) override { return host.allow_read && configuration==1; }
        bool use_interface(std::uint8_t number,std::uint8_t alternate) override { return host.allow_read && number==0 && alternate==0; }
        bool read_report(std::uint8_t number,std::vector<std::uint8_t>& bytes) override {
            if (number) return false; bytes={5,1,9,2}; return true;
        }
        bool set_report(std::uint8_t number,std::uint8_t type,std::uint8_t id,std::span<const std::uint8_t> bytes) override {
            if (number || type!=2 || id || bytes.size()!=2 || !host.allow_read) return false;
            ++host.output_reports; return true;
        }
    };
    std::unique_ptr<nt::usb::ControlPipe> open_control(const nt::usb::Device& device) override {
        if (!available || devices.empty() || device!=devices.front()) return {};
        return std::make_unique<Control>(*this);
    }
};

}
int main() {
    try {
        using namespace nt::iop;
        {
            std::vector<std::uint8_t> data(0x30000);
            auto cpu=initial(); cpu.pc=0x2c000; cpu.registers[29]=0x2f000; cpu.registers[16]=0x2c200;
            const auto result=run_native(nt_iop_generated_run,cpu,data,128);
            if (!(result.reason==NativeStop::unmapped_code && cpu.pc==0x5000 && cpu.registers[2]==87 &&
                  load_native_word(data,0x2c200)==0x2c100))
                throw std::runtime_error("Heap callback: stop="+std::to_string(static_cast<unsigned>(result.reason))+
                    " pc="+std::to_string(cpu.pc)+" value="+std::to_string(cpu.registers[2])+" target="+std::to_string(load_native_word(data,0x2c200)));
            cpu=initial(); cpu.pc=0x2c040; cpu.registers[29]=0x2f000;
            const auto visitor=run_native(nt_iop_generated_run,cpu,data,128);
            check(visitor.reason==NativeStop::unmapped_code && cpu.pc==0x5000 && cpu.registers[2]==93,
                  "Function-pointer argument saved across a helper call was not compiled");
        }
        {
            std::vector<std::uint8_t> data(0x30000);
            for (unsigned i=0;i<3;++i) store_native_word(data,0x26100+i*4,0x26060+i*8);
            for (const auto index:{0U,1U,2U,3U,0xffffffffU}) {
                auto cpu=initial(); cpu.pc=0x26000; cpu.registers[4]=index;
                const auto result=run_native(nt_iop_generated_run,cpu,data,64);
                check(result.reason==NativeStop::unmapped_code && cpu.pc==0x5000 &&
                      cpu.registers[2]==(index<3 ? 17+2*index : 91),
                      "Predecessor delay-slot table guard lost a case or accepted an out-of-range index");
            }
            store_native_word(data,0x26100,0x26110);
            auto cpu=initial(); cpu.pc=0x26000; cpu.registers[4]=0;
            const auto mutated=run_native(nt_iop_generated_run,cpu,data,64);
            if (mutated.reason!=NativeStop::unmapped_code || cpu.pc!=0x26038)
                throw std::runtime_error("Mutated edge-guarded table: stop="+std::to_string(static_cast<unsigned>(mutated.reason))+
                    " pc="+std::to_string(cpu.pc)+" address="+std::to_string(mutated.address));
        }
        {
            std::vector<std::uint8_t> data(0x30000);
            NativeHeapServices heap(data.size(),0x20000,0x24000);
            NativeInterruptServices interrupts(0x1f000);
            NativeThreadServices threads(interrupts);
            NativeFixedPoolServices pools(heap,&threads);
            NativeServiceRouter services({&pools,&threads});
            const auto call=[&](unsigned op,std::uint32_t a,std::uint32_t b=0,bool irq=false) {
                auto cpu=initial(); cpu.pc=0x25014+(op<=9 ? op-4 : op-5)*8;
                cpu.registers[4]=a; cpu.registers[5]=b; cpu.interrupt_depth=irq;
                const auto result=run_native(nt_iop_generated_run,cpu,data,32,&services);
                return std::pair{result,cpu.registers[2]};
            };
            store_native_word(data,0x18008,7); store_native_word(data,0x1800c,2);
            const auto pool=call(4,0x18000).second;
            const auto first=call(7,pool).second,second=call(8,pool,0,true).second;
            check(pool>0 && first==0x20000 && second==first+8 && call(7,pool).second==std::uint32_t(-400),
                  "Compiled fixed-pool allocation did not preserve stride or report exhaustion");
            data[first]=0xa5;
            check(call(9,pool,first+4).second==std::uint32_t(-426) && call(9,pool,first).second==0 &&
                  call(9,pool,first).second==std::uint32_t(-426) && call(7,pool).second==first && data[first]==0xa5,
                  "Fixed-pool ownership, reuse or payload preservation failed");
            auto waiting=initial(); waiting.pc=0x25024; waiting.registers[4]=pool; waiting.registers[31]=0xfffffffc;
            const auto one=threads.admit(waiting),two=threads.admit(waiting);
            check(threads.run(nt_iop_generated_run,data,64,services).reason==NativeStop::thread_wait,
                  "Exhausted fixed pool did not suspend native continuations");
            store_native_word(data,0x18058,0x12345678);
            check(call(11,pool,0x18040).second==0 && load_native_word(data,0x18050)==0 &&
                  load_native_word(data,0x18054)==2 && load_native_word(data,0x18058)==0x12345678,
                  "Fixed-pool status lost waiters or changed reserved fields");
            const auto before=data;
            check(call(11,pool,0x25014).first.reason==NativeStop::code_write && data==before &&
                  call(8,pool).second==std::uint32_t(-100),"Fixed-pool status or interrupt validation failed");
            check(call(9,pool,first).second==0 && threads.run(nt_iop_generated_run,data,64,services).reason==NativeStop::unmapped_code &&
                  threads.state(one).registers[2]==first,"FreeFpl did not hand the real block to its waiting continuation");
            check(call(5,pool).second==0 && threads.run(nt_iop_generated_run,data,64,services).reason==NativeStop::unmapped_code &&
                  threads.state(two).registers[2]==std::uint32_t(-425) &&
                  heap.invoke({8,{},NativeServiceKind::sysmem},data).value==0x4000,
                  "Pool deletion did not release storage and complete waiters with deletion errors");
            store_native_word(data,0x18000,0x200);
            const auto high=call(4,0x18000).second;
            check(call(7,high).second==0x23f00 && call(5,high).second==0,"High-memory fixed-pool placement failed");
            store_native_word(data,0x18008,0xffffffff);
            check(call(4,0x18000).second==std::uint32_t(-427),"Overflowing pool size was accepted");
        }
        {
            std::vector<std::uint8_t> data(0x30000),peer_data(0x2000);
            NativeSifDestination destination{peer_data,[](std::uint32_t,std::uint32_t) { return true; }};
            NativeSifTransfers transfers(&destination);
            NativeSifCommandEndpoint peer(destination,0x600,transfers);
            NativeSifInbound inbound(data,0x19000);
            NativeCallbackMux callbacks({});
            NativeInterruptServices interrupts(0x1f000,&callbacks);
            NativeThreadServices threads(interrupts);
            NativeHeapServices heap(data.size(),0x20000,0x24000);
            bool ready=false;
            NativeRpcClients clients(heap,threads,&peer,[&] { return ready; },data);
            callbacks=NativeCallbackMux({&inbound,&clients});
            NativeSifControl link(interrupts,&transfers);
            NativeEventServices events;
            NativeBootServices boot;
            NativeSystemServices system(events,boot,data);
            NativeSifCommandSession session(events,system,link,&peer);
            NativeSifCommands commands(&peer);
            inbound.connect(commands,session,link,nullptr,&clients);
            link.publish_main(0x600,0x10000); link.invoke({5,{},NativeServiceKind::sifman},data);
            NativeServiceRouter services({&clients,&threads,&interrupts});
            const auto call=[&](unsigned ordinal,std::uint32_t client,std::uint32_t mode) {
                auto cpu=initial(); cpu.pc=ordinal==15 ? 0x27014 : ordinal==16 ? 0x2701c : 0x27024;
                cpu.registers[4]=client; cpu.registers[5]=0x12345678; cpu.registers[6]=mode;
                cpu.registers[7]=0x18400; cpu.registers[29]=0x18300;
                const auto result=run_native(nt_iop_generated_run,cpu,data,32,&services);
                return std::pair{result,cpu.registers[2]};
            };
            const auto untouched=data;
            check(call(15,0x18000,1).first.reason==NativeStop::import_request && data==untouched && !peer.take(),
                  "RPC client bound before the real link was ready");
            ready=true;
            check(call(15,0x18000,0).first.reason==NativeStop::import_request && data==untouched,
                  "Synchronous RPC without a native continuation changed memory");
            auto waiting=initial(); waiting.pc=0x27014; waiting.registers[4]=0x18000;
            waiting.registers[5]=0x12345678; waiting.registers[31]=0xfffffffc;
            const auto bind_thread=threads.admit(waiting);
            check(threads.run(nt_iop_generated_run,data,64,services).reason==NativeStop::thread_wait &&
                  call(18,0x18000,0).second==1 && load_native_word(data,0x18024)==0,
                  "RPC bind reported completion without a peer response");
            const auto pending=data;
            check(call(15,0x18080,1).first.reason==NativeStop::thread_wait && data==pending &&
                  heap.invoke({8,{},NativeServiceKind::sysmem},data).value==0x3f00,
                  "RPC backpressure changed client state or leaked packet storage");
            const auto packet=peer.take();
            check(packet && load_native_word(*packet,32)==0x12345678,"RPC bind lost its requested server");
            auto reply=nt::sif::bind_reply(load_native_word(*packet,20),0x18000,0x900,0x1000);
            auto invalid=reply; store_native_word(invalid,20,0x20200);
            check(clients.receive(invalid,{}).status==NativeServiceStatus::unavailable && data==pending,
                  "Mismatched RPC reply completed a different transaction");
            check(inbound.submit(reply).value==1 && !inbound.take() && !inbound.failure() &&
                  call(18,0x18000,0).second==0 && load_native_word(data,0x18024)==0x900 &&
                  load_native_word(data,0x18014)==0x1000,
                  "Inbound bind reply did not publish the actual server and buffer");
            threads.run(nt_iop_generated_run,data,64,services);
            check(threads.state(bind_thread).registers[2]==0 &&
                  heap.invoke({8,{},NativeServiceKind::sysmem},data).value==0x4000,
                  "RPC bind did not resume its owner or release its packet");
            store_native_word(data,0x18310,4); store_native_word(data,0x18314,0x18100);
            store_native_word(data,0x18318,4); store_native_word(data,0x1831c,0x27100);
            store_native_word(data,0x18320,0x18200); store_native_word(data,0x18400,0x11223344);
            waiting.pc=0x2701c; waiting.registers[5]=7; waiting.registers[7]=0x18400;
            waiting.registers[28]=0x13579bdf; waiting.registers[29]=0x18300;
            const auto call_thread=threads.admit(waiting);
            check(threads.run(nt_iop_generated_run,data,64,services).reason==NativeStop::thread_wait,
                  "RPC call failed to wait for its result");
            const auto request=peer.take();
            check(request && load_native_word(*request,32)==7 && load_native_word(*request,52)==0x900 &&
                  load_native_word(peer_data,0x1000)==0x11223344 && load_native_word(data,0x18100)==0,
                  "RPC call lost its input payload or fabricated output");
            reply=nt::sif::bind_reply(load_native_word(*request,20),0x18000,0,0);
            store_native_word(reply,32,0x8000000a); store_native_word(reply,0,64|(4<<8));
            store_native_word(reply,4,0x18100);
            // A call completion does not echo the request's packet/id. Reused
            // reply storage can contain unrelated values in those fields.
            store_native_word(reply,20,0xdeadbeef); store_native_word(reply,24,0x12345678);
            const std::array<std::uint8_t,4> output{1,2,3,4};
            const auto before_completion=data;
            const auto request_id=load_native_word(data,0x18004);
            store_native_word(data,0x18004,request_id+1);
            check(clients.receive(reply,output).status==NativeServiceStatus::unavailable &&
                  load_native_word(data,0x18100)==0 && call(18,0x18000,0).second==1,
                  "RPC call completion released a changed client record");
            store_native_word(data,0x18004,request_id);
            invalid=reply; store_native_word(invalid,28,0x18080);
            check(clients.receive(invalid,output).status==NativeServiceStatus::unavailable && data==before_completion,
                  "RPC call completion accepted an unowned client");
            check(inbound.submit(reply,output).value==1 && !inbound.take() && !inbound.failure() &&
                  load_native_word(data,0x18100)==0x04030201 && call(18,0x18000,0).second==1 &&
                  load_native_word(data,0x18204)==0,"RPC completed before its compiled callback");
            interrupts.invoke({9,{},NativeServiceKind::intrman},data);
            threads.run(nt_iop_generated_run,data,128,services);
            check(load_native_word(data,0x18200)==0x13579bdf && load_native_word(data,0x18204)==77 &&
                  threads.state(call_thread).registers[2]==0 && threads.state(call_thread).registers[28]==0x13579bdf &&
                  call(18,0x18000,0).second==0,"RPC callback lost GP, output, completion ordering or its owner");
            check(clients.receive(reply,output).status==NativeServiceStatus::unavailable,
                  "Duplicate RPC call completion was accepted after release");
            store_native_word(data,0x18318,0); store_native_word(data,0x1831c,0);
            const auto no_output_thread=threads.admit(waiting);
            check(threads.run(nt_iop_generated_run,data,64,services).reason==NativeStop::thread_wait && peer.take(),
                  "RPC call without output did not suspend its owner");
            reply.fill(0); store_native_word(reply,0,64); store_native_word(reply,8,0x80000008);
            store_native_word(reply,28,0x18000); store_native_word(reply,32,0x8000000a);
            check(inbound.submit(reply).value==1 && !inbound.take() && !inbound.failure() &&
                  call(18,0x18000,0).second==0,"RPC completion required undefined echoed fields");
            threads.run(nt_iop_generated_run,data,64,services);
            check(threads.state(no_output_thread).registers[2]==0,
                  "RPC completion without output did not resume its owner");
            check(call(15,0x18080,1).second==0 && call(18,0x18080,0).second==1,
                  "Asynchronous bind did not remain pending");
            const auto asynchronous=peer.take();
            reply=nt::sif::bind_reply(load_native_word(*asynchronous,20),0x18080,0,0);
            check(clients.receive(reply,{}).status==NativeServiceStatus::returned && call(18,0x18080,0).second==0 &&
                  load_native_word(data,0x180a4)==0,"Missing server reply fabricated a binding");
        }
        {
            NativeJumpServices jumps;
            std::vector<std::uint8_t> data(0x20000);
            auto cpu=initial(); cpu.pc=0x17400; cpu.registers[28]=0x12345678; cpu.registers[29]=0x1fff0;
            NativeResult result;
            do { result=run_native(nt_iop_generated_run,cpu,data,1,&jumps); } while (result.reason==NativeStop::budget && cpu.executed<100);
            check(result.reason==NativeStop::unmapped_code && cpu.pc==0x5000 && cpu.registers[2]==1 &&
                  cpu.registers[3]==42 && cpu.registers[16]==42 && cpu.registers[28]==0x12345678 &&
                  load_native_word(data,0x18000)==0x17418 && load_native_word(data,0x1802c)==0x12345678,
                  "Compiled setjmp/longjmp failed register restoration, zero normalization or sliced execution");
            const auto before=data;
            for (const auto address:{0x1fff0U,0x17000U}) {
                cpu=initial(); cpu.pc=0x1705c; cpu.registers[4]=address;
                result=run_native(nt_iop_generated_run,cpu,data,64,&jumps);
                check(result.reason==(address==0x17000 ? NativeStop::code_write : NativeStop::address_fault) && data==before,
                      "Invalid jump buffer changed memory");
            }
            cpu=initial(); cpu.pc=0x17064; cpu.registers[4]=0x80018000; cpu.registers[5]=7;
            check(run_native(nt_iop_generated_run,cpu,data,64,&jumps).reason==NativeStop::unmapped_code &&
                  cpu.pc==0x5000 && cpu.registers[2]==7,"Longjmp lost nonzero return value or pointer alias");
            cpu=initial(); cpu.pc=0x17064; cpu.registers[4]=0x18000; cpu.native_call_owner=&jumps;
            check(run_native(nt_iop_generated_run,cpu,data,64,&jumps).reason==NativeStop::import_request && data==before,
                  "Longjmp abandoned an owned native callback");
            store_native_word(data,0x18000,0x18040);
            cpu=initial(); cpu.pc=0x17064; cpu.registers[4]=0x18000;
            result=run_native(nt_iop_generated_run,cpu,data,64,&jumps);
            check(result.reason==NativeStop::unmapped_code && result.address==0x18040,
                  "Longjmp accepted an uncompiled continuation");
        }
        for (const bool interrupt_rotation:{false,true}) {
            std::vector<std::uint8_t> data(0x20000);
            NativeInterruptServices interrupts(0x1f000);
            NativeThreadServices threads(interrupts);
            NativeServiceRouter services({&threads,&interrupts});
            auto first=initial(); first.pc=interrupt_rotation ? 0x12080 : 0x17500; first.registers[31]=0xfffffffc;
            auto second=initial(); second.pc=0x12040; second.registers[31]=0xfffffffc;
            const auto first_id=threads.admit(first),second_id=threads.admit(second);
            check(threads.invoke({15,{second_id,3},NativeServiceKind::thbase},data).value==static_cast<unsigned>(-100) &&
                  threads.invoke({15,{0,3},NativeServiceKind::thbase,0,true},data).value==static_cast<unsigned>(-406),
                  "Interrupt priority change accepted invalid context or thread id");
            check(threads.invoke({15,{second_id,9},NativeServiceKind::thbase,0,true},data).value==0 &&
                  threads.wait_priority(second_id)==9 &&
                  threads.invoke({15,{second_id,8},NativeServiceKind::thbase,0,true},data).value==0,
                  "Interrupt priority change did not update the native scheduler");
            if (interrupt_rotation) {
                check(interrupts.invoke({4,{4,0,0x17580,0},NativeServiceKind::intrman},data).value==0 &&
                      interrupts.invoke({6,{4},NativeServiceKind::intrman},data).value==0 &&
                      interrupts.invoke({9,{},NativeServiceKind::intrman},data).value==0,"Rotation interrupt setup failed");
                interrupts.post(4);
            }
            const auto result=threads.run(nt_iop_generated_run,data,64,services);
            check(result.reason==NativeStop::unmapped_code && threads.state(second_id).registers[2]==87 &&
                  threads.state(first_id).pc==(interrupt_rotation ? 0x12080U : 0x17510U) &&
                  !threads.state(first_id).interrupt_depth,"Ready-queue rotation lost FIFO order or switched inside a handler");
            check(threads.run(nt_iop_generated_run,data,64,services).reason==NativeStop::unmapped_code &&
                  threads.state(first_id).registers[2]==(interrupt_rotation ? 99U : 42U),
                  "Rotated thread did not resume its compiled continuation");
        }
        {
            std::vector<std::uint8_t> data(0x25000);
            NativeLibrarySpec declaration{0x24000,0x109,"dev9",{},0x24100};
            for (unsigned ordinal=0;ordinal<14;++ordinal)
                declaration.functions.push_back(0x24100+(ordinal==3 ? 1U : ordinal)*8);
            NativeLibraryServices libraries({declaration});
            NativeDev9Services dev9(libraries);
            NativeModloadServices loader;
            NativeServiceRouter services({&dev9,&libraries,&loader});
            const auto snapshot=data;
            auto cpu=initial(); cpu.pc=0x24100;
            const NativeModule module{0x24000,0x24100,0x200,0x200,0};
            const auto id=loader.begin_startup(module);
            const auto result=run_native(nt_iop_generated_run,cpu,data,64,&services);
            check(result.reason==NativeStop::unmapped_code && cpu.pc==0x5000 && cpu.registers[2]==1 &&
                  dev9.unavailable_initializations()==1 && data==snapshot && !libraries.resolve("dev9",0x109,4),
                  "Unavailable DEV9 adapter fabricated initialization, memory writes or registered exports");
            loader.finish_startup(id,cpu.registers[2]);
            check(loader.invoke({17,{id,0},NativeServiceKind::modload},data).value==static_cast<unsigned>(-202),
                  "Unavailable DEV9 adapter remained resident");
            for (const auto ordinal:{4U,5U,7U,8U,9U,12U,13U}) {
                cpu=initial(); cpu.pc=0x24100+ordinal*8;
                check(run_native(nt_iop_generated_run,cpu,data,64,&services).reason==NativeStop::import_request && data==snapshot,
                      "Unimplemented DEV9 export fabricated a successful operation");
            }
            check(dev9.invoke({0x100,{},NativeServiceKind::dev9,0x24108},data).status==NativeServiceStatus::unavailable &&
                  dev9.invoke({0x100,{},NativeServiceKind::dev9,0x24100,true},data).status==NativeServiceStatus::unavailable &&
                  dev9.unavailable_initializations()==1,"DEV9 accepted an invalid initialization context");
        }
        {
            NativeModloadServices loader;
            std::vector<std::uint8_t> data(0x20000,0);
            const NativeModule module{0xb000,0xb280,0x400,0x600,0xb300,NativeModuleKind::compiled,
                {"synthetic-module",0x102,0x300,0x100,0x200}};
            const auto call=[&](unsigned pc,std::array<std::uint32_t,4> args) {
                auto cpu=initial(); cpu.pc=pc; std::copy(args.begin(),args.end(),cpu.registers.begin()+4);
                const auto result=run_native(nt_iop_generated_run,cpu,data,64,&loader);
                return std::pair{result,cpu.registers[2]};
            };
            const auto count=[&] { return load_native_word(data,0x18100); };
            const auto empty=call(0xb434,{0xffffffff,0,0x18100});
            check(empty.second==0 && count()==0,"Catalog-free native module enumeration was not empty");
            const auto first=loader.begin_startup(module),second=loader.begin_startup(module);
            const auto limited=call(0xb434,{0xa0018000,1,0x80018100});
            check(limited.first.reason==NativeStop::unmapped_code && limited.second==1 && count()==2 &&
                load_native_word(data,0x18000)==first && load_native_word(data,0x18004)==0,
                "Module list ignored capacity, count or pointer aliases");
            std::fill_n(data.begin()+0x18200,96,std::uint8_t{0xcd});
            check(call(0xb43c,{first,0x18200}).second==0 && load_native_word(data,0x18238)==0x00020102 &&
                load_native_word(data,0x1823c)==first && load_native_word(data,0x18240)==0xb280 &&
                load_native_word(data,0x18250)==0x100 && load_native_word(data,0x18254)==0x200 &&
                data[0x18258]==0xcd,"Starting module status lost metadata or overwrote reserved fields");
            std::copy(module.details.name.begin(),module.details.name.end(),data.begin()+0x18300);
            check(call(0xb44c,{0x18300}).second==second && call(0xb454,{0xa000b5ff}).second==second &&
                call(0xb454,{0xb600}).second==static_cast<unsigned>(-202),"Module search failed name, alias or extent semantics");
            loader.finish_startup(first,0); loader.finish_startup(second,1);
            check(call(0xb444,{0x18300,0x18000,2,0x18100}).second==1 && count()==1 &&
                call(0xb43c,{first,0x18200}).second==0 && load_native_word(data,0x18238)==0x00030102,
                "Module completion did not publish residency or remove a released entry");
            const auto before=data;
            check(call(0xb434,{0x18000,1,0xb434}).first.reason==NativeStop::code_write && data==before,
                "Invalid module count output partially wrote the ID list");
            check(call(0xb43c,{first,0x1fffc}).first.reason==NativeStop::address_fault && data==before,
                "Truncated module status partially changed memory");
            check(call(0xb43c,{second,0xffffffff}).second==static_cast<unsigned>(-202) && data==before,
                "Released module status touched output or reported success");
            data[0x18300]='x';
            check(call(0xb444,{0x18300,0xffffffff,1,0x18100}).second==0 && count()==0,
                "Missing name produced fabricated module residency");
        }
        {
            std::vector<std::uint8_t> data(0x20000);
            NativeSysclibServices strings; NativeCdvdmanServices disc;
            NativeServiceRouter router({&strings,&disc});
            const auto call=[&](unsigned pc,std::array<std::uint32_t,4> args) {
                auto cpu=initial(); cpu.pc=pc; std::copy(args.begin(),args.end(),cpu.registers.begin()+4);
                const auto result=run_native(nt_iop_generated_run,cpu,data,64,&router);
                return std::pair{result,cpu};
            };
            for (unsigned i=0;i<16;++i) data[0x18000+i]=static_cast<std::uint8_t>(i+1);
            check(call(0x17014,{0x80018000,0xa0018002,8}).first.reason==NativeStop::unmapped_code &&
                  data[0x18002]==1 && data[0x18009]==8,"Compiled bcopy lost argument order, aliases or overlap semantics");
            data[0x18108]=0xcc;
            const auto word_copy=call(0x1701c,{0x80018100,0xa0018000,11});
            check(word_copy.first.reason==NativeStop::unmapped_code && word_copy.second.registers[2]==0x80018100 &&
                  std::equal(data.begin()+0x18000,data.begin()+0x18008,data.begin()+0x18100) && data[0x18108]==0xcc,
                  "Word copy failed to truncate its byte count or preserve alias return");
            for (const auto args:{std::array{0x18101U,0x18000U,8U,0U},std::array{0x18100U,0x18001U,8U,0U},
                std::array{0x1fffcU,0x18000U,8U,0U}}) {
                const auto before=data;
                check(call(0x1701c,args).first.reason==NativeStop::address_fault && data==before,"Rejected word copy partially changed memory");
            }
            const auto before=data;
            check(call(0x17014,{0x18000,0x15080,4}).first.reason==NativeStop::code_write && data==before,
                  "bcopy wrote into compiled code storage");
            check(call(0x1701c,{0xffffffff,0xffffffff,3}).first.reason==NativeStop::unmapped_code,
                  "Empty word copy dereferenced unused pointers");
            const std::string source="abc.def.abc",pattern=".d";
            std::copy(source.begin(),source.end(),data.begin()+0x18200);
            std::copy(pattern.begin(),pattern.end(),data.begin()+0x18300);
            check(call(0x17024,{0x18200,0x18300}).second.registers[2]==3 &&
                  call(0x1702c,{0x80018200,0x18300}).second.registers[2]==0x80018203 &&
                  call(0x17034,{0x18200,0x18300}).second.registers[2]==0 &&
                  call(0x1703c,{0xa0018200,0x18300}).second.registers[2]==0xa0018203,
                  "Compiled string search/span family returned the wrong position");
            data[0x18300]=0;
            check(call(0x1703c,{0xffffffff,0x18300}).second.registers[2]==0xffffffff &&
                  call(0x17024,{0x18200,0x18300}).second.registers[2]==source.size(),"Empty pattern semantics changed");
            data.back()='x';
            check(call(0x1703c,{0x18200,0x1ffff}).first.reason==NativeStop::address_fault,"Unterminated pattern escaped bounds");
            check(call(0x17114,{0x18000,0x18100}).first.reason==NativeStop::import_request,
                  "Console iLink identity was fabricated");
            store_native_word(data,0x18038,0x18100); store_native_word(data,0x18108,0x12040);
            check(call(0x11200,{0x18000}).second.registers[2]==87,"Nested descriptor callback did not reach compiled client");
            store_native_word(data,0x18108,0x12080);
            check(call(0x11200,{0x18000}).second.registers[2]==99,
                  "Cross-library callback argument was absent from the compiled registry candidates");
            store_native_word(data,0x18108,0x12040);
            check(call(0x11240,{0x18000}).second.registers[2]==87,"Conditional descriptor callback did not reach compiled client");
            store_native_word(data,0x18108,0);
            check(call(0x11240,{0x18000}).second.registers[2]==0,"Null descriptor callback invoked code");
            check(call(0x11280,{0,0x150c0}).second.pc==0x5000,
                  "An assigned callback was restricted to incidental relocation candidates");
            check(call(0x11280,{0,0x16700}).first.address==0x16700,
                  "Assigned callback accepted a destination outside compiled code");
            store_native_word(data,0x17600,0x17330); store_native_word(data,0x17604,0x1733c);
            const auto cursor=call(0x17300,{});
            check(cursor.first.reason==NativeStop::unmapped_code && cursor.second.pc==0x5000 && cursor.second.registers[2]==30,
                  "Compiled cursor jump table did not follow its checked induction variable");
            store_native_word(data,0x18108,0x16700);
            check(call(0x11200,{0x18000}).first.address==0x16700,"Nested descriptor accepted uncompiled target");
        }
        {
            std::vector<std::uint8_t> data(0x20000);
            for (const auto budget : {1U,128U}) for (unsigned index=0;index<=7;++index) {
                auto cpu=initial(); cpu.pc=0x17700; cpu.registers[4]=index;
                NativeResult result;
                do { result=nt_iop_generated_run(cpu,data,budget); } while (result.reason==NativeStop::budget);
                unsigned expected=259;
                for (unsigned suffix=0;suffix<index;++suffix) expected+=128U>>suffix;
                if (index<=6)
                    check(result.reason==NativeStop::unmapped_code && cpu.pc==0x5000 &&
                          cpu.registers[2]==expected && cpu.executed==9+index,
                          "Compiled inline tail lost a selected operation or delay slot");
                else
                    check(result.reason==NativeStop::unmapped_code && cpu.pc==0x17714 &&
                          result.address==0x17718 && cpu.registers[2]==1 && cpu.executed==5 && !cpu.delay_slot,
                          "Inline tail admitted an out-of-range but compiled destination");
            }
            store_native_word(data,0x17b00,0x17a54); store_native_word(data,0x17b04,0x17a60);
            for (unsigned index=0;index<3;++index) {
                auto cpu=initial(); cpu.pc=0x17a00; cpu.registers[4]=index; cpu.registers[29]=0x1fff0;
                const auto result=nt_iop_generated_run(cpu,data,128);
                check(result.reason==NativeStop::unmapped_code && cpu.pc==0x5000 && cpu.registers[29]==0x1fff0 &&
                      cpu.registers[2]==(index==0 ? 7U : index==1 ? 9U : 0xffffffffU),
                      "Stack-spill prologue hid a bounded jump table");
            }
        }
        {
            std::vector<std::uint8_t> data(0x20000);
            NativeLibrarySpec declaration{0x14000,0x102,"usbd",{},0x14200};
            store_native_word(data,0x14000,0x41c00000); store_native_word(data,0x14008,0x102);
            const std::string name="usbd"; std::copy(name.begin(),name.end(),data.begin()+0x1400c);
            for (unsigned ordinal=0;ordinal<19;++ordinal) {
                const auto entry=0x14100+(ordinal==0 || ordinal==3 ? 1U : ordinal)*8;
                declaration.functions.push_back(entry); store_native_word(data,0x14014+ordinal*4,entry);
            }
            NativeLibraryServices libraries({declaration},{{0x152f4,0x152e0,"usbd",0x102,4},{0x152fc,0x152e0,"usbd",0x102,11}});
            NativeHeapServices heap(0x20000,0x18000,0x1f000);
            FixtureUsb host; NativeUsbServices usb(libraries,heap,host);
            const auto initial_devices=host.devices;
            NativeInterruptServices usb_interrupts(0x17f00);
            NativeThreadServices usb_threads(usb_interrupts,&heap);
            usb.connect(usb_threads);
            NativeServiceRouter router({&libraries,&usb,&usb_threads});
            const auto call=[&](unsigned pc,std::array<std::uint32_t,4> arguments={}) {
                auto cpu=initial(); cpu.pc=pc; cpu.registers[28]=0x16230; cpu.registers[29]=0x15600;
                std::copy(arguments.begin(),arguments.end(),cpu.registers.begin()+4);
                const auto result=run_native(nt_iop_generated_run,cpu,data,256,&router);
                return std::pair{result,cpu};
            };
            check(call(0x14200).first.reason==NativeStop::import_request && !libraries.resolve("usbd",0x102,4),
                  "USB startup fabricated a host snapshot");
            host.available=true;
            check(call(0x14200).first.reason==NativeStop::unmapped_code && usb.device_count()==1 &&
                  libraries.resolve("usbd",0x102,4)==0x14120,"USB module startup did not publish real descriptor ownership");
            const auto device=call(0x14130,{1,0,1});
            check(device.second.registers[2]==0x18000 && data[0x18000]==18,"USB device descriptor missing");
            check(call(0x14130,{1,0x18000,2}).second.registers[2]==0x18012 &&
                  call(0x14130,{1,0x18001,0}).second.registers[2]==0,"USB descriptor iterator accepted a non-boundary pointer");
            store_native_word(data,0x15448,0x15480); data[0x15480]='t';
            store_native_word(data,0x1544c,0x15080); store_native_word(data,0x15450,0x150a0); store_native_word(data,0x15454,0x150c0);
            const auto registration=call(0x15000);
            check(registration.first.reason==NativeStop::unmapped_code && registration.second.pc==0x5000 &&
                  registration.second.registers[2]==0 && usb.driver_count()==1 &&
                  load_native_word(data,0x15400)==1 && load_native_word(data,0x15404)==1 &&
                  load_native_word(data,0x15408)==0x16230,"Compiled USB probe/connect sequence or GP restoration failed");
            check(call(0x14120,{0x15440}).second.registers[2]==0x121,"Duplicate USB driver registration succeeded");
            check(call(0x14138,{1,0x1234}).second.registers[2]==0 && call(0x14140,{1}).second.registers[2]==0x1234,
                  "USB private data did not retain device ownership");
            check(call(0x14128,{0x80015440}).second.registers[2]==0 && !usb.driver_count() &&
                  call(0x14140,{1}).second.registers[2]==0,"USB unregistration failed to release device ownership");
            store_native_word(data,0x1544c,0x150c0); // Compiled rejection callback.
            check(call(0x14120,{0x15440}).second.registers[2]==0 && usb.driver_count()==1 &&
                  load_native_word(data,0x15404)==1,"Rejected USB probe still ran connect");
            check(call(0x14128,{0x15440}).second.registers[2]==0,"Rejected-probe registration could not be removed");
            store_native_word(data,0x1544c,0);
            check(call(0x14120,{0x15440}).first.reason==NativeStop::unmapped_code && usb.driver_count()==1,
                  "USB registration without callbacks did not complete synchronously");
            check(call(0x14128,{0x15440}).second.registers[2]==0,"Empty registration could not be removed");
            store_native_word(data,0x1544c,0x15080);
            store_native_word(data,0x16300,0x16080); store_native_word(data,0x16304,0x16090);
            for (unsigned index=0;index<3;++index)
                check(call(0x16000,{index}).second.registers[2]==(index ? 11U : 10U),"Compiled hoisted indexed jump table selected the wrong target");
            check(call(0x14158,{1}).second.registers[2]==0x102,"Unknown USB pipe was accepted");
            const auto pipe=call(0x14160,{1}).second.registers[2];
            check(pipe==1 && host.opened==1 && call(0x14148,{99}).second.registers[2]==0xffffffff,
                  "USB control pipe did not acquire actual host ownership");
            store_native_word(data,0x15610,0x15100); store_native_word(data,0x15614,0x15700);
            store_native_word(data,0x15800,0x01000680); store_native_word(data,0x15804,18<<16);
            const auto snapshot=data;
            host.allow_read=false;
            check(call(0x14158,{pipe,0x15900,18,0x15800}).first.reason==NativeStop::import_request && data==snapshot && usb.error()==5,
                  "Failed USB host read changed memory or completed successfully");
            host.allow_read=true;
            check(call(0x14158,{pipe,0x15901,18,0x15800}).second.registers[2]==0x106 && host.reads==1,
                  "Aligned USB pipe issued a misaligned transfer");
            check(call(0x14158,{pipe,0x15100,18,0x15800}).first.reason==NativeStop::code_write && host.reads==1,
                  "USB transfer overwrote compiled code");
            for (unsigned i=0;i<2;++i)
                check(call(i ? 0x14158 : 0x15140,{pipe,0x15900,18,0x15800}).second.registers[2]==0,"Native USB descriptor transfer failed");
            check(data[0x15900]==18 && data[0x15901]==1 && load_native_word(data,0x1570c)==0 && host.reads==3,
                  "USB read lost data or executed its callback inline");
            check(usb_threads.run(nt_iop_generated_run,data,256,router).reason==NativeStop::thread_wait &&
                  load_native_word(data,0x15700)==0 && load_native_word(data,0x15704)==18 &&
                  load_native_word(data,0x15708)==0x16230 && load_native_word(data,0x1570c)==2 &&
                  usb_threads.state(1).interrupt_depth==0,
                  "USB foreground callbacks lost ordering, length, caller GP or normal thread context");
            store_native_word(data,0x15800,0x00010900); store_native_word(data,0x15804,0);
            check(call(0x14158,{pipe,0,0,0x15800}).second.registers[2]==0,
                  "USB did not adopt an active host configuration");
            usb_threads.run(nt_iop_generated_run,data,128,router);
            check(load_native_word(data,0x15704)==0 && load_native_word(data,0x1570c)==3,
                  "Zero-length USB configuration lost deferred completion");
            store_native_word(data,0x15800,0x00000b01);
            check(call(0x14158,{pipe,0,0,0x15800}).second.registers[2]==0,"USB interface selection failed");
            usb_threads.run(nt_iop_generated_run,data,128,router);
            check(load_native_word(data,0x1570c)==4,"USB interface callback was not delivered");
            store_native_word(data,0x15610,0); // This pair needs no completion callback.
            store_native_word(data,0x15800,0x22000681); store_native_word(data,0x15804,3<<16);
            check(call(0x14158,{pipe,0x15980,3,0x15800}).second.registers[2]==0 && data[0x15980]==5 && data[0x15982]==9,
                  "Native HID report descriptor did not honor requested length");
            store_native_word(data,0x15800,0x02000921); store_native_word(data,0x15804,2<<16);
            check(call(0x14158,{pipe,0x15980,2,0x15800}).second.registers[2]==0 && host.output_reports==1,
                  "HID output report did not reach host service");
            store_native_word(data,0x15610,0x15100); store_native_word(data,0x15804,0);
            store_native_word(data,0x15800,0x00020900);
            check(call(0x14158,{pipe,0,0,0x15800}).first.reason==NativeStop::import_request,
                  "USB invented a host configuration that was not active");
            check(call(0x14150,{pipe}).second.registers[2]==0 && host.closed==1 &&
                  call(0x14150,{pipe}).second.registers[2]==0x102,"USB close did not release its host pipe");
            check(call(0x14168,{1,0x15920}).second.registers[2]==0 && data[0x15920]==2 && data[0x15921]==3 && data[0x15926]==0,
                  "USB device location lost the real port chain");
            const auto endpoint=call(0x14130,{1,0,5}).second.registers[2];
            host.allow_input=false;
            check(call(0x14160,{1,endpoint}).second.registers[2]==0xffffffff,"Host access denial fabricated an input pipe");
            host.input_error=50;
            check(call(0x14160,{1,endpoint}).first.reason==NativeStop::import_request,
                  "Missing USB backend was disguised as a physical-device failure");
            host.allow_input=true;
            const auto input=call(0x14160,{1,endpoint}).second.registers[2];
            check(input==2 && call(0x14158,{input,0x15940,8,0}).second.registers[2]==0 &&
                  call(0x14158,{input,0x15940,8,0}).second.registers[2]==0x121,"USB input did not retain a pending host read");
            usb.take();
            check(data[0x15940]==0 && !usb.failure(),"Pending USB read fabricated bytes");
            host.input_complete=true; usb.take(); usb_threads.run(nt_iop_generated_run,data,128,router);
            check(data[0x15940]==3 && data[0x15942]==8 && load_native_word(data,0x15704)==3 && load_native_word(data,0x1570c)==5,
                  "Actual USB completion lost bytes or compiled callback");
            host.input_complete=false;
            check(call(0x14158,{input,0x15940,8,0}).second.registers[2]==0 && call(0x14150,{input}).second.registers[2]==0 && host.cancelled==1,
                  "USB close did not cancel the outstanding host request");
            usb_threads.run(nt_iop_generated_run,data,128,router);
            check(load_native_word(data,0x15700)==0x122 && load_native_word(data,0x15704)==0,"Cancelled USB callback reported success");
            host.devices.clear();
            check(call(0x14120,{0x15440}).first.reason==NativeStop::import_request && !usb.driver_count(),
                  "USB registration concealed a host topology change");
            host.devices=initial_devices;
            store_native_word(data,0x1544c,0x16700);
            check(call(0x14120,{0x15440}).first.reason==NativeStop::unmapped_code && !usb.driver_count(),
                  "Uncompiled USB probe was accepted");
            // Odd descriptor lengths must not misalign the next native device
            // declaration. Exercise generated startup and descriptor imports.
            host.devices={initial_devices.front(),initial_devices.front(),initial_devices.front()};
            host.devices[1].key=L"second-synthetic-port";
            host.devices[2].key=L"third-synthetic-port";
            NativeLibraryServices aligned_libraries({declaration});
            NativeHeapServices aligned_heap(0x20000,0x18000,0x1f000);
            NativeUsbServices aligned_usb(aligned_libraries,aligned_heap,host);
            auto startup=initial(); startup.pc=0x14200;
            check(run_native(nt_iop_generated_run,startup,data,8,&aligned_usb).reason==NativeStop::unmapped_code &&
                  aligned_usb.device_count()==3,"Multi-device USB startup failed");
            std::uint32_t previous_end=0;
            for (unsigned id=1;id<=3;++id) {
                auto query=initial(); query.pc=0x14130; query.registers[4]=id; query.registers[5]=0; query.registers[6]=1;
                check(run_native(nt_iop_generated_run,query,data,8,&aligned_usb).reason==NativeStop::unmapped_code,
                      "Compiled multi-device descriptor query failed");
                const auto address=query.registers[2];
                const auto& bytes=host.devices[id-1].descriptors;
                check(address%4==0 && address>=previous_end && address<data.size() && bytes.size()<=data.size()-address &&
                      std::equal(bytes.begin(),bytes.end(),data.begin()+address),
                      "USB descriptor allocation lost alignment, separation or wire bytes");
                previous_end=address+static_cast<std::uint32_t>(bytes.size());
            }
        }
        {
            std::vector<std::uint8_t> data(0x20000);
            NativeLibrarySpec declaration{0x13000,0x104,"libsd",{}};
            store_native_word(data,0x13000,0x41c00000); store_native_word(data,0x13008,0x104);
            const std::string name="libsd";
            std::copy(name.begin(),name.end(),data.begin()+0x1300c);
            for (unsigned ordinal=0;ordinal<28;++ordinal) {
                const auto entry=0x13100+(ordinal==3 ? 1U : ordinal)*8;
                declaration.functions.push_back(entry); store_native_word(data,0x13014+ordinal*4,entry);
            }
            NativeLibraryServices libraries({declaration});
            FixtureAudio output;
            NativeSoundServices sound(libraries,output);
            NativeHeapServices sound_heap(0x20000,0x1b000,0x1f000); sound.connect(sound_heap);
            const auto call=[&](unsigned ordinal,unsigned argument=0,unsigned value=0,unsigned third=0,unsigned fourth=0,unsigned fifth=0) {
                auto cpu=initial(); cpu.pc=declaration.functions.at(ordinal); cpu.registers[4]=argument;
                cpu.registers[5]=value; cpu.registers[6]=third; cpu.registers[7]=fourth;
                cpu.registers[29]=0x17e00; cpu.registers[28]=0x123456; store_native_word(data,0x17e10,fifth);
                store_native_word(data,0x17e14,0x19c00);
                const auto stop=run_native(nt_iop_generated_run,cpu,data,32,&sound);
                return std::pair{stop,cpu};
            };
            const auto failed=call(0);
            check(failed.first.reason==NativeStop::import_request && !failed.second.executed &&
                  !libraries.resolve("libsd",0x104,4) && !output.ready(),"Sound startup fabricated a working device");
            output.allow_open=true;
            store_native_word(data,0x13008,0x105);
            check(call(0).first.reason==NativeStop::import_request && !output.ready(),"Invalid sound declaration retained a native device");
            store_native_word(data,0x13008,0x104);
            check(call(0).first.reason==NativeStop::unmapped_code && output.ready() && !output.voices() &&
                  libraries.resolve("libsd",0x104,4)==0x13120,"Native sound startup failed to register after opening its device");
            check(call(4).first.reason==NativeStop::unmapped_code && output.voices()==48 && output.initializations==1,
                  "Compiled sound initialization did not allocate native voices");
            check(call(4,1).first.reason==NativeStop::unmapped_code && output.initializations==2,"Warm initialization did not reset native voices");
            for (unsigned core=0;core<2;++core) {
                check(call(5,0x980|core,0x2000).first.reason==NativeStop::unmapped_code &&
                      call(5,0xa80|core,0x4000).first.reason==NativeStop::unmapped_code &&
                      output.core_mix[core].output.left==0.5f && output.core_mix[core].output.right==-1.0f,
                      "Master controls did not reach independent native stereo buses");
                check(call(6,0x980|core).second.registers[2]==0x2000,"Master readback lost the library value");
                const auto voice=core*24+3;
                check(call(5,6|core,0x2000).first.reason==NativeStop::unmapped_code &&
                      call(5,0x106|core,0x3fff).first.reason==NativeStop::unmapped_code &&
                      call(5,0x206|core,0x800).first.reason==NativeStop::unmapped_code &&
                      output.voice_mix[voice].direct.left==0.5f && output.voice_mix[voice].rate==0.5f,
                      "Voice panning and pitch did not reach the native voice");
                check(call(7,0x1800|core,0xfffff7).first.reason==NativeStop::unmapped_code &&
                      output.voice_mix[voice].direct.left==0 && output.voice_mix[voice].direct.right>0.99f &&
                      call(8,0x1800|core).second.registers[2]==0xfffff7,"Native per-channel voice routing was not applied");
                check(call(5,0x800|core,0).first.reason==NativeStop::unmapped_code &&
                      output.voice_mix[voice].direct.right==0,"Core mixer gate did not silence its native route");
                check(call(9,0x1d00|core,0x17ffff).first.reason==NativeStop::unmapped_code &&
                      call(10,0x1d00|core).second.registers[2]==0x17ffff &&
                      call(25,core,0,4).first.reason==NativeStop::unmapped_code,"Effect workspace initialization failed");
                for (unsigned aliases:{0U,0x40U,0x80U,0xc0U})
                    for (unsigned kind:{0x2000U,0x2100U}) {
                        const auto selector=kind|aliases|6U|core;
                        check(call(9,selector,0x812f).first.reason==NativeStop::unmapped_code &&
                              call(10,kind|6U|core).second.registers[2]==0x8120 &&
                              call(10,selector).second.registers[2]==0x8120,
                              "Compiled sound address import lost selector aliases or block alignment");
                    }
                check(call(9,0x2030|core,0x8000).first.reason==NativeStop::import_request &&
                      call(9,0x2006|core,0x200000).first.reason==NativeStop::import_request,
                      "Invalid voice or sample address was accepted");
            }
            store_native_word(data,0x18000,0); store_native_word(data,0x18004,0);
            check(call(23,1,0x18000).first.reason==NativeStop::unmapped_code &&
                  call(24,1,0x18020).first.reason==NativeStop::unmapped_code && load_native_word(data,0x18020)==1,
                  "Disabled effect configuration did not retain the selected core");
            for (unsigned mode : {1U,2U,3U,4U,5U,6U,9U}) {
                store_native_word(data,0x18004,mode|0x100); store_native_word(data,0x18008,0xc0002000);
                check(call(23,1,0x18000).first.reason==NativeStop::unmapped_code &&
                      call(24,1,0x18020).first.reason==NativeStop::unmapped_code &&
                      load_native_word(data,0x18024)==(mode|0x100) && load_native_word(data,0x18028)==0xc0002000 &&
                      output.core_mix[1].reverb.depth.left==0.25f && output.core_mix[1].reverb.depth.right==-0.5f,
                      "Compiled effect configuration did not reach the native stereo reverb bus");
            }
            check(output.reverb_clears==7 && call(11,3,1).first.reason==NativeStop::unmapped_code &&
                  output.core_mix[1].reverb.enabled && output.core_mix[1].reverb.space==nt::audio::ReverbSpace::pipe,
                  "Native effect activation or history clearing failed");
            check(call(5,0xb81,0x4000).first.reason==NativeStop::unmapped_code && output.core_mix[1].reverb.depth.left==0.5f,
                  "Effect depth parameter did not update the native bus");
            check(call(25,1,0,9).first.reason==NativeStop::unmapped_code && output.reverb_clears==8,
                  "Effect work-area clear retained native delay history");
            output.allow_configure=false; store_native_word(data,0x18004,4);
            check(call(23,1,0x18000).first.reason==NativeStop::import_request &&
                  call(24,1,0x18020).first.reason==NativeStop::unmapped_code && load_native_word(data,0x18024)==0x109,
                  "Rejected native effect configuration changed public state");
            output.allow_configure=true;
            for (unsigned mode : {7U,8U}) {
                store_native_word(data,0x18004,mode);
                check(call(23,1,0x18000).first.reason==NativeStop::import_request,"Unimplemented echo/delay returned success");
            }
            store_native_word(data,0x18004,10);
            check(call(23,1,0x18000).second.registers[2]==0xffffffff,"Invalid reverb mode did not return its documented error");
            check(call(23,1,0x1fff8).first.reason==NativeStop::address_fault && call(24,1,0x13100).first.reason==NativeStop::code_write,
                  "Effect attribute bounds or code protection failed");
            check(call(26,0,0x13108,0x18000).first.reason==NativeStop::unmapped_code &&
                  call(26,0,0).second.registers[2]==0x13108 &&
                  call(27,0x1ff00).first.address==0x1ff00,
                  "Sound callback replacement or AOT validation failed");
            check(call(8,0x1700).second.registers[2]==0 && call(7,0x1600,0xffffff).first.reason==NativeStop::unmapped_code,
                  "Idle voice status or key-off failed");
            for (unsigned i=0;i<64;++i) data[0x19000+i]=static_cast<std::uint8_t>(i*3);
            check(call(26,0,0x13108,0x1aa00).first.reason==NativeStop::unmapped_code &&
                  call(17,0,0,0x19000,0x8000,64).second.registers[2]==64 && call(19,0,1).second.registers[2]==1,
                  "Sample transfer did not complete a native copy");
            const auto callback=sound.take();
            check(callback && callback->address==0x13108 && callback->argument==0 && callback->second_argument==0x1aa00 &&
                  callback->global_pointer==0x123456,"Sample completion lost callback ownership or GP");
            sound.finish(0);
            check(call(17,1,1,0x19100,0x8000,64).second.registers[2]==64 &&
                  std::equal(data.begin()+0x19000,data.begin()+0x19040,data.begin()+0x19100),"Sample readback lost transferred data");
            check(call(17,1,1,0x13100,0x8000,64).first.reason==NativeStop::code_write,"Sound transfer overwrote compiled code");
            check(call(11,4,1).first.reason==NativeStop::unmapped_code && call(12,4).second.registers[2]==1 &&
                  call(11,8,25).first.reason==NativeStop::unmapped_code && call(12,8).second.registers[2]==25,
                  "Core controls lost native service state");
            check(call(26,0,0).first.reason==NativeStop::unmapped_code,"Transfer handler removal failed");
            for (unsigned i=0;i<256;++i) { data[0x19400+i*2]=7; data[0x19600+i*2]=11; }
            check(call(18,0,0,0x19400,1024).second.registers[2]==0 && output.streams[0].queued &&
                  output.pcm[0][0]==7 && output.pcm[0][1]==11,"Planar PCM did not become native interleaved audio");
            check(call(18,0,0,0x19400,1024).second.registers[2]==0xffffffff,"Active stream was overwritten");
            output.streams[0]={256,false}; sound.take();
            check(call(20,0).second.registers[2]==0,"Completed PCM retained a busy status");
            check(call(18,0,2).first.reason==NativeStop::unmapped_code && output.pcm[0].empty(),"Stream cancellation retained native PCM");
            check(call(18,0,0x80,0x19400,1024).first.reason==NativeStop::unmapped_code,"Uncompiled refill callback was accepted");
            store_native_word(data,0x19c00,0x19400); store_native_word(data,0x19c04,1024);
            check(call(18,0,0x80,0x19400,1024,0x15180).second.registers[2]==0,"Compiled refill stream failed to start");
            NativeInterruptServices sound_interrupts(0x1a000,&sound);
            sound_interrupts.invoke({9,{},NativeServiceKind::intrman},data);
            auto interrupted=initial(); interrupted.pc=0x5000;
            output.streams[0].frames+=256; output.streams[0].queued=false;
            sound_interrupts.run(nt_iop_generated_run,interrupted,data,128,sound);
            check(output.streams[0].queued && load_native_word(data,0x19c08)==0x123456 && !sound.failure(),
                  "Compiled four-argument refill did not submit its next buffer with the registering GP");
            output.streams[0].frames+=256; output.streams[0].queued=false;
            sound_interrupts.run(nt_iop_generated_run,interrupted,data,128,sound);
            check(!sound.failure() && call(20,0).second.registers[2]==0,"Empty refill output did not stop native playback");
            output.allow_configure=false;
            check(call(5,0x980,0).first.reason==NativeStop::import_request &&
                  call(6,0x980).second.registers[2]==0x2000,"Failed native volume update reported success");
            output.allow_configure=true;
            const auto snapshot=data;
            check(call(5,0x980,0x8000).first.reason==NativeStop::import_request && data==snapshot && output.initializations==2,
                  "Unimplemented sound operation returned success or changed state");
            check(call(4,2).first.reason==NativeStop::import_request && output.initializations==2,"Unsupported sound mode changed output");
            output.allow_initialize=false;
            check(call(4).first.reason==NativeStop::import_request && output.initializations==2,"Failed voice allocation fabricated completion");
            check(call(2).first.reason==NativeStop::unmapped_code && !output.ready() && !output.voices(),"Sound quit retained native resources");
            output.allow_initialize=true;
            check(call(4,1).first.reason==NativeStop::unmapped_code && call(6,0x980).second.registers[2]==0 &&
                  call(10,0x1d01).second.registers[2]==0x1fffff,"Warm open after quit retained stale sound state");
        }
        {
            std::vector<std::uint8_t> data(0x10000);
            NativeModloadServices loader;
            const auto call=[&](std::uint32_t entry,std::array<std::uint32_t,4> args={}) {
                auto cpu=initial(); cpu.pc=entry;
                std::copy(args.begin(),args.end(),cpu.registers.begin()+4);
                return run_native(nt_iop_generated_run,cpu,data,128,&loader);
            };
            check(call(0xb424,{0xc000,0xc004}).reason==NativeStop::unmapped_code &&
                  load_native_word(data,0xc000)==0 && load_native_word(data,0xc004)==0,
                  "Unregistered loader callbacks were fabricated");
            check(call(0xb200).reason==NativeStop::unmapped_code && call(0xb240).reason==NativeStop::unmapped_code,
                  "Compiled loader callback registration failed");
            check(call(0xb424,{0x8000c000,0xa000c004}).reason==NativeStop::unmapped_code &&
                  load_native_word(data,0xc000)==0xb298 && load_native_word(data,0xc004)==0xb290,
                  "Loader query lost registered callback identity");
            for (const auto output : {0x10000U,0xb000U}) {
                const auto before=data;
                check(call(0xb424,{0xc000,output}).reason==
                      (output==0xb000 ? NativeStop::code_write : NativeStop::address_fault) && data==before,
                      "Rejected callback query partially wrote its outputs");
            }
            check(call(0xb414,{0xb280,0xffff,0}).reason==NativeStop::unmapped_code &&
                  call(0xb424,{0xc000,0xc004}).reason==NativeStop::unmapped_code && load_native_word(data,0xc004)==0xb290,
                  "Invalid loader callback partially changed registration");
            {
                auto cpu=initial(); cpu.pc=0xb42c;
                const auto loading=run_native(nt_iop_generated_run,cpu,data,128,&loader);
                check(loading.reason==NativeStop::unmapped_code && cpu.pc==0x5000 &&
                      cpu.registers[2]==static_cast<std::uint32_t>(-201),
                      "Empty LoadStartModule path invented a module id");
            }
            {
                NativeModloadServices boot;
                std::vector<std::uint8_t> paths(0x10000,0);
                const auto place=[&](std::uint32_t address,std::string_view text) {
                    check(static_cast<std::size_t>(address)+text.size()<paths.size(),"Boot-device fixture does not fit");
                    std::copy(text.begin(),text.end(),paths.begin()+address);
                };
                place(0x3000,"cdrom0:\\MODULE.IRX");
                place(0x3100,"cdrom:\\MODULE.IRX");
                place(0x3200,"rom0:MODULE");
                place(0x3300,"rom:MODULE");
                place(0x3400,"rom3:MODULE");
                place(0x3500,"cdrom1:\\MODULE.IRX");
                place(0x3600,"host:\\MODULE.IRX");
                place(0x3700,"mc0:FILE");
                place(0x3800,"MODULE.IRX");
                place(0x3900," cdrom0:\\MODULE.IRX");
                place(0x3a00,"cdrom00:\\MODULE.IRX");
                place(0x3b00,"rom4:MODULE");
                place(0x3c00,"CDROM0:\\MODULE.IRX");
                place(0x3d00,"");
                std::fill_n(paths.begin()+0x4000,256,'A');
                NativeServiceRequest query;
                query.kind=NativeServiceKind::modload;
                query.selector=15;
                const auto ask=[&](std::uint32_t address) {
                    query.arguments[0]=address;
                    query.interrupt_context=false;
                    return boot.invoke(query,paths);
                };
                const auto before=paths;
                check(ask(0).status==NativeServiceStatus::address_fault && ask(0x80000000).status==NativeServiceStatus::address_fault &&
                      paths==before && boot.boot_device_decision()==-1,
                      "Null boot-device path returned a decision");
                check(ask(0x4000).status==NativeServiceStatus::returned && ask(0x4000).value==static_cast<std::uint32_t>(-36) &&
                      boot.boot_device_decision()==-1,"Unterminated boot-device path returned legal");
                std::vector<std::uint8_t> tail(32,'A');
                query.arguments[0]=20;
                check(boot.invoke(query,tail).status==NativeServiceStatus::address_fault,
                      "Boot-device path running off RAM returned a decision");
                const auto legal=[&](std::uint32_t address,const char* device) {
                    const auto reply=ask(address);
                    check(reply.status==NativeServiceStatus::returned && reply.value==0 &&
                          boot.boot_device()==device && boot.boot_device_decision()==0,
                          "Native boot device was rejected");
                };
                legal(0x3000,"cdrom");
                check(boot.boot_device_path()=="cdrom0:\\MODULE.IRX","Boot-device query dropped the guest path");
                legal(0x3100,"cdrom"); legal(0x3200,"rom"); legal(0x3300,"rom"); legal(0x3400,"rom");
                const auto illegal=[&](std::uint32_t address) {
                    const auto reply=ask(address);
                    check(reply.status==NativeServiceStatus::returned && reply.value==1 && boot.boot_device_decision()==1,
                          "Illegal boot device was accepted");
                };
                illegal(0x3500); illegal(0x3600); illegal(0x3700); illegal(0x3800); illegal(0x3900);
                illegal(0x3a00); illegal(0x3b00); illegal(0x3c00); illegal(0x3d00);
                check(boot.boot_device_queries()>=1,"Boot-device query was not counted");
                query.interrupt_context=true;
                const auto queries=boot.boot_device_queries();
                check(boot.invoke(query,paths).status==NativeServiceStatus::unavailable && boot.boot_device_queries()==queries,
                      "Interrupt boot-device query changed the decision");
                query.interrupt_context=false;
                query.selector=14;
                query.arguments={0x5000,0x5004,0,0};
                check(boot.invoke(query,paths).status==NativeServiceStatus::returned &&
                      load_native_word(paths,0x5000)==0 && load_native_word(paths,0x5004)==0,
                      "Boot-device query invented loader callback state");
            }
            for (const auto callback : {0xb280U,0xb288U,0xb290U,0xb298U}) {
                auto cpu=initial(); cpu.pc=callback;
                check(nt_iop_generated_run(cpu,data,8).reason==NativeStop::unmapped_code &&
                      cpu.pc==0x5000 && cpu.registers[2]==0x60+(callback-0xb280)/8,
                      "Registered loader callback was not compiled");
            }
            check(call(0xb494).pc==0x5000 && call(0xb49c).pc==0x5000,
                  "Native cache publication did not return to compiled code");
            auto callback=initial(); callback.pc=0xb500; callback.registers[29]=0xc100;
            check(run_native(nt_iop_generated_run,callback,data,128,&loader).reason==NativeStop::unmapped_code &&
                  callback.pc==0x5000 && callback.registers[2]==0x73 && callback.registers[29]==0xc100,
                  "Stack callback lost its native target across a helper call");
            callback=initial(); callback.pc=0xb500; callback.registers[29]=0xc100;
            check(run_native(nt_iop_generated_run,callback,data,7,&loader).reason==NativeStop::budget,
                  "Stack callback fixture did not stop after publication");
            store_native_word(data,0xc0f0,0xb000);
            check(run_native(nt_iop_generated_run,callback,data,128,&loader).reason==NativeStop::unmapped_code &&
                  callback.pc==0xb528,"Stack callback accepted an unlisted compiled target");
            callback=initial(); callback.pc=0xb588;
            check(nt_iop_generated_run(callback,data,8).reason==NativeStop::unmapped_code &&
                  callback.pc==0x5000 && callback.registers[2]==0x74,
                  "Callback stored in a call delay slot was not compiled");
            callback=initial(); callback.pc=0xb5e8;
            check(nt_iop_generated_run(callback,data,8).reason==NativeStop::unmapped_code &&
                  callback.pc==0x5000 && callback.registers[2]==0x75,
                  "Callback passed to a compiled library was not compiled");
        }
        {
            std::vector<std::uint8_t> data(0x10000);
            for (unsigned i=0;i<3;++i) store_native_word(data,0xb300+i*4,0xb080+i*16);
            for (const auto entry : {0xb000U,0xb180U,0xb320U,0xb380U}) for (unsigned selector=0;selector<4;++selector) {
                auto cpu=initial(); cpu.pc=entry; cpu.registers[4]=selector;
                check(nt_iop_generated_run(cpu,data,128).reason==NativeStop::unmapped_code && cpu.pc==0x5000,
                      "Compiled callback selector did not return");
                if (selector==3) { check(cpu.registers[2]==0,"Selector default did not return null"); continue; }
                check(cpu.registers[2]==0xb100+selector*8,"Selector returned the wrong callback");
                cpu.pc=cpu.registers[2];
                check(nt_iop_generated_run(cpu,data,16).reason==NativeStop::unmapped_code &&
                      cpu.pc==0x5000 && cpu.registers[2]==0x50+selector,
                      "Returned callback was not discovered and executed as LLVM code");
            }
            store_native_word(data,0xb300,0xb100); // Compiled, but not an allowed table destination.
            for (auto entry : {0xb180U,0xb320U,0xb380U}) {
                auto changed=initial(); changed.pc=entry; changed.registers[4]=0;
                check(nt_iop_generated_run(changed,data,128).reason==NativeStop::unmapped_code && changed.pc!=0x5000,
                      "Jump table accepted a mutated destination outside its candidate set");
            }
        }
        // No guest instruction bytes are present in RAM. Only LLVM-compiled code runs.
        for (const auto alias : {0U,0x80000000U,0xa0000000U}) for (const auto budget : {1U,2U,7U,100U}) {
            std::vector<std::uint8_t> ram(0x10000);
            store_native_word(ram,0x2000,100);
            auto cpu=initial(alias); NativeResult result{};
            unsigned slices=0;
            do { result=nt_iop_generated_run(cpu,ram,budget); }
            while (result.reason==NativeStop::budget && ++slices<100);
            check(result.reason==NativeStop::unmapped_code && cpu.pc==0x5000 && cpu.executed==20 &&
                  cpu.registers[2]==100 && cpu.registers[3]==99 && cpu.registers[5]==106 &&
                  cpu.registers[6]==7 && cpu.registers[9]==6 && cpu.registers[10]==3 &&
                  load_native_word(ram,0x2004)==106,"Compiled IOP flow, memory, alias or load-delay result differs");
        }
        std::vector<std::uint8_t> ram(0x10000);
        {
            NativeHeapServices backing(0x10000,0x6000,0x9000);
            NativeHeapLibraryServices heaps(backing);
            const auto call=[&](unsigned ordinal,std::uint32_t a=0,std::uint32_t b=0) {
                auto cpu=initial(); cpu.pc=0x1ac0+(ordinal-4)*8; cpu.registers[4]=a; cpu.registers[5]=b;
                check(run_native(nt_iop_generated_run,cpu,ram,1,&heaps).reason==NativeStop::budget && cpu.pc==0x5000,
                      "Compiled heaplib import failed");
                return cpu.registers[2];
            };
            const auto available=[&] { return backing.invoke({8,{},NativeServiceKind::sysmem},ram).value; };
            const auto original=available();
            const auto fixed=call(4,512,0),growing=call(4,512,3);
            check(fixed==0x6000 && growing==0x8e00 && call(8,fixed)==472 && available()==original-1024,
                  "Heap creation lost placement, backing ownership or capacity");
            const auto first=call(6,fixed,9),second=call(6,fixed,0);
            check(first && second && first%8==0 && second%8==0 && first!=second && call(8,fixed)==432,
                  "Heap allocation lost eight-byte accounting or minimum allocation");
            store_native_word(ram,first,0x12345678);
            check(call(7,growing,first)==0xffffffff && load_native_word(ram,first)==0x12345678 &&
                  call(7,fixed,first+8)==0xffffffff,"Heap free accepted another arena or an interior pointer");
            check(call(7,fixed,first)==0 && call(7,fixed,first)==0xffffffff && call(7,fixed,second)==0 &&
                  call(8,fixed)==472,"Heap free did not coalesce or rejected repeated-free detection");
            check(call(6,fixed,600)==0,"Fixed heap grew silently");
            const auto large=call(6,growing,600);
            check(large && available()==original-1792 && call(8,growing)==600,"Growing heap accounting differs");
            check(call(7,growing,large)==0 && available()==original-1024 && call(8,growing)==472,
                  "Empty growth block was not returned to the shared allocator");
            check(call(6,growing,0xffffffff)==0 && call(8,0)==std::uint32_t(-4),"Invalid heap request wrapped or succeeded");
            auto truncated=initial(); truncated.pc=0x1ac8; truncated.registers[4]=fixed;
            check(run_native(nt_iop_generated_run,truncated,std::span(ram).first(0x8000),1,&heaps).reason==NativeStop::address_fault &&
                  !truncated.executed && available()==original-1024,"Truncated arena partially deleted a heap");
            call(6,fixed,16); call(5,fixed); call(5,growing);
            check(available()==original && call(7,fixed,first)==std::uint32_t(-4),"Heap deletion leaked blocks or retained a handle");
        }
        {
            NativeHeapServices callback_heap(0x10000,0xd000,0xe000);
            NativeBootCallbacks callbacks(callback_heap);
            NativeServiceRouter services({&callbacks});
            store_native_word(ram,0xc700,0xc800); store_native_word(ram,0xc704,0x12345678);
            for (const auto priority : {3U,1U,0U,2U}) {
                auto registration=initial(); registration.pc=0x1a70; registration.registers[5]=priority;
                registration.registers[6]=0xc704; registration.registers[28]=priority*16;
                const auto registered=run_native(nt_iop_generated_run,registration,ram,4,&services);
                check(registered.reason==NativeStop::budget &&
                      registration.registers[2]==1 && load_native_word(ram,0xc704)==0x12345678,
                      "Deferred startup callback executed or wrote immediate status at registration");
            }
            Cpu completion; completion.registers[29]=0xf000;
            NativeResult result{};
            for (unsigned slice=0;slice<100 && !callbacks.complete();++slice)
                result=callbacks.finish_startup(nt_iop_generated_run,completion,ram,1,services);
            check(callbacks.complete() && result.reason==NativeStop::unmapped_code && completion.registers[29]==0xf000,
                  "Startup callback sequence failed to complete across slices");
            for (unsigned i=0;i<4;++i) check(load_native_word(ram,0xc800+i*12)==i*16 &&
                load_native_word(ram,0xc804+i*12)==1 && load_native_word(ram,0xc808+i*12)==0x2ec3,
                "Startup callback priority, GP, delayed flag or next record differs");
            auto immediate=initial(); immediate.pc=0x1a70; immediate.registers[6]=0xc704; immediate.registers[28]=99;
            check(run_native(nt_iop_generated_run,immediate,ram,64,&services).reason==NativeStop::unmapped_code &&
                  immediate.pc==0x5000 && immediate.registers[2]==0 && load_native_word(ram,0xc704)==std::uint32_t(-7) &&
                  load_native_word(ram,0xc830)==99 && load_native_word(ram,0xc834)==0 && load_native_word(ram,0xc838)==1 &&
                  callback_heap.invoke({8,{},NativeServiceKind::sysmem},ram).value==0x1000,
                  "Immediate startup callback lost result, context or scratch ownership");
            auto invalid=initial(); invalid.pc=0x1534; invalid.registers[4]=0x4000;
            check(run_native(nt_iop_generated_run,invalid,ram,1,&services).reason==NativeStop::unmapped_code && !invalid.executed,
                  "Uncompiled boot callback was accepted");
            invalid.registers[4]=0x2ec0; invalid.registers[6]=0x1000;
            check(run_native(nt_iop_generated_run,invalid,ram,1,&services).reason==NativeStop::code_write && !invalid.executed &&
                  callback_heap.invoke({8,{},NativeServiceKind::sysmem},ram).value==0x1000,
                  "Invalid immediate status destination allocated or ran a callback");
        }
        for (unsigned i=0;i<3;++i) store_native_word(ram,0x1fb0+i*4,0x2f68+i*8);
        for (const auto index : {0U,1U,2U,3U,0xffffffffU}) {
            auto selected=initial(); selected.pc=0x1fc0; selected.registers[4]=index;
            check(nt_iop_generated_run(selected,ram,64).reason==NativeStop::unmapped_code && selected.pc==0x5000 &&
                  selected.registers[2]==(index<3 ? (index+1)*11 : 99),"Compiled switch table selected the wrong branch");
        }
        store_native_word(ram,0x1fb0,0x17c0); // Compiled code, but outside this switch's candidate set.
        auto changed_switch=initial(); changed_switch.pc=0x1fc0; changed_switch.registers[4]=0;
        check(nt_iop_generated_run(changed_switch,ram,64).reason==NativeStop::unmapped_code && changed_switch.pc==0x1fe0,
              "Modified jump table escaped its runtime target guard");
        store_native_word(ram,0x1fb0,0x80002f68);
        changed_switch=initial(); changed_switch.pc=0x1fc0; changed_switch.registers[4]=0;
        check(nt_iop_generated_run(changed_switch,ram,64).reason==NativeStop::unmapped_code && changed_switch.pc==0x5000 &&
              changed_switch.registers[2]==11,"Aliased switch target did not reach its compiled branch");
        for (unsigned index=0;index<3;++index) store_native_word(ram,0xa580+index*4,0xa500+index*8);
        for (unsigned variant=0;variant<4;++variant) {
            const auto entry=0xa140+variant*0x80;
            for (const auto index : {0U,1U,2U,3U,0xffffffffU}) {
                auto selected=initial(); selected.pc=entry; selected.registers[4]=index;
                check(nt_iop_generated_run(selected,ram,64).reason==NativeStop::unmapped_code && selected.pc==0x5000 &&
                      selected.registers[2]==(index<3 ? (index+1)*11 : 99) && (index>=3 || selected.registers[7]==17),
                      "Compiled call/switch dispatch lost its delay slot, default or return continuation");
            }
            store_native_word(ram,0xa580,0xa100); // Valid compiled entry, outside this table's candidates.
            auto changed=initial(); changed.pc=entry; changed.registers[4]=0;
            const auto transfer=entry+((variant&1) ? 32 : 36);
            check(nt_iop_generated_run(changed,ram,64).reason==NativeStop::unmapped_code && changed.pc==transfer &&
                  changed.registers[31]==0x5000 && changed.registers[7]==0,
                  "Changed call table ran an unapproved destination or retired its delay slot");
            store_native_word(ram,0xa580,0xa500);
        }
        for (unsigned operation=0;operation<4;++operation) for (unsigned lane=0;lane<4;++lane)
            for (const auto alias : {0U,0x80000000U,0xa0000000U}) {
                const bool load=operation<2,left=(operation&1)==0;
                constexpr std::uint32_t original=0xaabbccdd,pending=0x11223344,contents=0x76543210;
                store_native_word(ram,0xb800,contents);
                auto state=initial(); state.pc=0xa380+operation*16; state.registers[4]=alias|0xb800|lane;
                state.registers[2]=original; state.load_register=2; state.load_value=pending;
                auto expected=load ? pending : contents;
                const auto source=load ? contents : original;
                for (unsigned byte=0;byte<(left ? lane+1 : 4-lane);++byte) {
                    const auto source_byte=load ? (left ? byte : lane+byte) : (left ? 3-lane+byte : byte);
                    const auto destination_byte=load ? (left ? 3-lane+byte : byte) : (left ? byte : lane+byte);
                    expected=(expected&~(255U<<(destination_byte*8)))|(((source>>(source_byte*8))&255U)<<(destination_byte*8));
                }
                check(nt_iop_generated_run(state,ram,1).reason==NativeStop::budget && state.executed==1 &&
                      (load ? state.load_register==2 && state.load_value==expected && state.registers[2]==original :
                              load_native_word(ram,0xb800)==expected && state.registers[2]==pending),
                      "Partial word access lost byte lanes, load forwarding or alias semantics");
                auto fault=initial(); fault.pc=0xa380+operation*16; fault.registers[4]=0xb803;
                fault.delay_slot=true; fault.branch_target=0x5000; fault.load_register=2; fault.load_value=pending;
                check(nt_iop_generated_run(fault,std::span(ram).first(0xb803),1).reason==NativeStop::address_fault &&
                      fault.executed==0 && fault.delay_slot && fault.load_register==2,
                      "Truncated partial word access retired state on fault");
                if (!load) {
                    fault=initial(); fault.pc=0xa380+operation*16; fault.registers[4]=0xa381;
                    check(nt_iop_generated_run(fault,ram,1).reason==NativeStop::code_write && !fault.executed,
                          "Partial word store overwrote compiled code");
                }
            }
        for (unsigned reverse=0;reverse<2;++reverse) for (unsigned lane=0;lane<4;++lane) {
            for (unsigned byte=0;byte<8;++byte) ram[0xb800+byte]=static_cast<std::uint8_t>(byte+1);
            auto state=initial(); state.pc=0xa3c0+reverse*0x20; state.registers[4]=0xb800+lane;
            check(nt_iop_generated_run(state,ram,32).reason==NativeStop::unmapped_code && state.pc==0x5000 &&
                  state.registers[2]==load_native_word(ram,0xb800+lane) && state.registers[3]==99,
                  "Paired partial loads lost merge forwarding or architectural load delay");
        }
        const NativeLibrarySpec declared{0x1214,0x100,"fixture",{0x10f0,0x1108,0x1140}};
        const NativeLibrarySpec upgraded{0x1440,0x102,"fixture",{0x1360}};
        const auto declaration=[&](const NativeLibrarySpec& library) {
            store_native_word(ram,library.address,0x41c00000);
            store_native_word(ram,library.address+4,0); store_native_word(ram,library.address+8,library.version);
            for (unsigned n=0;n<8;++n) ram[library.address+12+n]=n<library.name.size() ? static_cast<std::uint8_t>(library.name[n]) : 0;
            for (unsigned n=0;n<library.functions.size();++n) store_native_word(ram,library.address+20+n*4,library.functions[n]);
            store_native_word(ram,library.address+20+static_cast<unsigned>(library.functions.size())*4,0);
        };
        declaration(declared); declaration(upgraded);
        NativeLibraryServices libraries({declared,upgraded});
        NativeHeapServices auxiliary_heap(0x10000,0x6000,0x9000);
        NativeServiceRouter router({&auxiliary_heap,&libraries});
        const auto library_call=[&](unsigned ordinal,std::uint32_t address) {
            auto cpu=initial(); cpu.pc=ordinal==6 ? 0x13b0 : 0x13b8; cpu.registers[4]=address;
            const auto stop=run_native(nt_iop_generated_run,cpu,ram,1,&router);
            check(stop.reason==NativeStop::budget && cpu.executed==1 && cpu.pc==0x5000,"Compiled library call did not retire and return");
            return cpu.registers[2];
        };
        check(!libraries.resolve("fixture",0x100,0),"Discovered exports were implicitly registered");
        check(library_call(6,0x80001214)==0 && libraries.resolve("fixture",0x1ff,1)==0x1108 &&
              !libraries.resolve("fixture",0x200,0) && !libraries.resolve("fixture",0x100,3),"Native library registration or major/ordinal resolution differs");
        check(library_call(6,0x1214)==static_cast<std::uint32_t>(-212),"Duplicate library registration succeeded");
        auto exported=initial(); exported.pc=*libraries.resolve("fixture",0x100,0); exported.registers[4]=8; exported.registers[5]=2;
        check(nt_iop_generated_run(exported,ram,10).reason==NativeStop::unmapped_code && exported.registers[3]==32,
              "Registered export did not execute compiled function");
        check(library_call(6,0x1440)==0 && libraries.resolve("fixture",0x100,0)==0x1360 &&
              library_call(6,0x1214)==static_cast<std::uint32_t>(-212),"Newer library revision did not take precedence");
        check(library_call(7,0x1440)==0 && libraries.resolve("fixture",0x100,0)==0x10f0 &&
              library_call(7,0x1214)==0 && !libraries.resolve("fixture",0x100,0) &&
              library_call(7,0x1214)==static_cast<std::uint32_t>(-213),"Library release or previous revision restoration differs");
        store_native_word(ram,0x1228,0x4000);
        check(library_call(6,0x1214)==static_cast<std::uint32_t>(-214) && !libraries.resolve("fixture",0x100,0),
              "Mutated export declaration introduced an uncompiled target");
        declaration(declared);
        check(library_call(6,0x1800)==static_cast<std::uint32_t>(-214),"Unknown export table was registered");
        auto invalid_library=initial(); invalid_library.pc=0x13b0; invalid_library.registers[4]=0x200000;
        check(run_native(nt_iop_generated_run,invalid_library,ram,1,&router).reason==NativeStop::address_fault &&
              !invalid_library.executed && !libraries.resolve("fixture",0x100,0),"Invalid export pointer changed native registry");
        const auto reboot_mode=[&](unsigned address,unsigned mode) {
            auto state=initial(); state.pc=0x1524; state.registers[4]=address; state.registers[5]=mode;
            check(run_native(nt_iop_generated_run,state,ram,1,&router).reason==NativeStop::budget &&
                  state.pc==0x5000,"Library reboot mode call did not return through compiled import");
            return state.registers[2];
        };
        check(reboot_mode(0x80001214,0xffffffff)==0 && load_native_word(ram,0x121c)==0x60100 &&
              !libraries.resolve("fixture",0x100,0),"Reboot mode did not mask flags or implicitly activated exports");
        check(library_call(6,0x1214)==0 && libraries.resolve("fixture",0x100,0)==0x10f0 &&
              reboot_mode(0x1214,2)==0 && load_native_word(ram,0x121c)==0x20100 &&
              libraries.resolve("fixture",0x100,0)==0x10f0,"Mode update broke registration or active bindings");
        check(reboot_mode(0,6)==static_cast<std::uint32_t>(-214) &&
              reboot_mode(0x1800,6)==static_cast<std::uint32_t>(-213),"Invalid reboot-mode table succeeded");
        store_native_word(ram,0x1228,0x4000);
        check(reboot_mode(0x1214,4)==static_cast<std::uint32_t>(-214) && load_native_word(ram,0x121c)==0x20100,
              "Mode update accepted a changed compiled export declaration");
        store_native_word(ram,0x1228,0x10f0);
        check(reboot_mode(0x1214,0)==0 && load_native_word(ram,0x121c)==0x100 && library_call(7,0x1214)==0,
              "Mode clearing/release lost registry ownership");
        auto truncated_mode=initial(); truncated_mode.pc=0x1524; truncated_mode.registers[4]=0x1214;
        const auto before_mode=ram;
        check(run_native(nt_iop_generated_run,truncated_mode,std::span(ram).first(0x1220),1,&router).reason==NativeStop::address_fault &&
              !truncated_mode.executed && ram==before_mode,"Truncated mode descriptor partially changed state");
        // Library query reads a bounded identity and returns the registered
        // compiled export array, with the same version selection as resolution.
        store_native_word(ram,0x1808,0x1ff);
        for (unsigned n=0;n<8;++n) ram[0x180c+n]=n<declared.name.size() ? static_cast<std::uint8_t>(declared.name[n]) : 0;
        const auto library_query=[&]() {
            auto cpu=initial(); cpu.pc=0x1514; cpu.registers[4]=0x1800;
            check(run_native(nt_iop_generated_run,cpu,ram,1,&libraries).reason==NativeStop::budget,"Compiled library query did not return");
            return cpu.registers[2];
        };
        check(library_query()==0 && library_call(6,declared.address)==0 && library_query()==declared.address+20,
              "Library query ignored registration state or returned a header instead of exports");
        check(library_call(6,upgraded.address)==0 && library_query()==upgraded.address+20 &&
              library_call(7,upgraded.address)==0 && library_query()==declared.address+20,
              "Library query did not track version precedence and release");
        store_native_word(ram,0x1808,0x200); check(library_query()==0,"Library query crossed ABI major versions");
        check(library_call(7,declared.address)==0,"Queried library could not be released");
        auto short_query=initial(); short_query.pc=0x1514; short_query.registers[4]=0xfffc;
        check(run_native(nt_iop_generated_run,short_query,ram,1,&libraries).reason==NativeStop::address_fault && !short_query.executed,
              "Truncated library identity was read or retired");

        const std::array modes{NativeBootMode{0x1234,4,{0xabcdef01,0x23456789}},NativeBootMode{7,5,{}},NativeBootMode{9,4,{}}};
        NativeBootServices boot(modes,ram,0xb000,256), empty_boot;
        const auto boot_query=[&](NativeBootServices& service,std::uint32_t id) {
            auto cpu=initial(); cpu.pc=0x151c; cpu.registers[4]=id;
            check(run_native(nt_iop_generated_run,cpu,ram,1,&service).reason==NativeStop::budget && cpu.pc==0x5000 && cpu.executed==1,
                  "Compiled boot query did not retire and return");
            return cpu.registers[2];
        };
        check(boot_query(empty_boot,4)==0 && boot_query(boot,4)==0xb000 && boot_query(boot,5)==0xb00c &&
              boot_query(boot,6)==0 && boot_query(boot,0x104)==0,"Boot query ignored context, duplicate order or full-width mode ID");
        check(load_native_word(ram,0xb000)==0x02041234 && load_native_word(ram,0xb004)==0xabcdef01 &&
              load_native_word(ram,0xb008)==0x23456789,"Native boot record ABI layout differs");
        auto read_boot=initial(); read_boot.pc=0x1018; read_boot.registers[4]=boot_query(boot,4);
        check(nt_iop_generated_run(read_boot,ram,2).reason==NativeStop::budget && read_boot.registers[2]==0x02041234,
              "Compiled code could not read a returned boot record");
        const auto before_bad_context=ram;
        bool rejected_context=false;
        try { NativeBootServices invalid(modes,ram,0xb000,8); }
        catch (const std::invalid_argument&) { rejected_context=true; }
        check(rejected_context && ram==before_bad_context,"Invalid boot configuration partially changed RAM");
        store_native_word(ram,0xb000,0xff041234);
        auto malformed_boot=initial(); malformed_boot.pc=0x151c; malformed_boot.registers[4]=4;
        check(run_native(nt_iop_generated_run,malformed_boot,ram,1,&boot).reason==NativeStop::address_fault && !malformed_boot.executed,
              "Mutated boot-record length escaped its reserved extent");
        store_native_word(ram,0xb000,0x02041234);
        const NativeLibrarySpec worker{0xa020,0x100,"worker",{0xa000}};
        declaration(worker);
        NativeLibraryServices linked({worker},{{0x13dc,0x13c8,"worker",0x103,0}});
        const auto control=[&](unsigned ordinal,std::uint32_t address,std::uint32_t size=0) {
            auto cpu=initial(); cpu.pc=ordinal==6 ? 0x13b0 : ordinal==7 ? 0x13b8 : ordinal==8 ? 0x1418 : ordinal==10 ? 0x152c : 0x1420;
            cpu.registers[4]=address; cpu.registers[5]=size;
            const auto stop=run_native(nt_iop_generated_run,cpu,ram,1,&linked);
            check(stop.reason==NativeStop::budget && cpu.executed==1,"Compiled link control did not return");
            return cpu.registers[2];
        };
        auto client=initial(); client.pc=0x13ec; client.registers[4]=37;
        check(run_native(nt_iop_generated_run,client,ram,50,&linked).reason==NativeStop::import_request && client.pc==0x13dc && client.executed==3,
              "Unregistered cross-module import silently executed");
        check(control(6,worker.address)==0,"Provider registration did not bind known compiled imports");
        NativeResult linked_stop{};
        do { linked_stop=run_native(nt_iop_generated_run,client,ram,1,&linked); } while (linked_stop.reason==NativeStop::budget);
        check(linked_stop.reason==NativeStop::unmapped_code && client.pc==0x5000 && client.executed==10 && client.registers[2]==42,
              "Cross-module native call lost its arguments, return address or continuation");
        check(control(7,worker.address)==static_cast<std::uint32_t>(-215),"Provider released while imports remained bound");
        check(control(9,0x13c8,36)==0 && control(7,worker.address)==0 && control(8,0x13c8,36)==0xffffffff,
              "Unlink, release or unresolved relink status differs");
        check(control(6,worker.address)==0,"Provider could not register again");
        client=initial(); client.pc=0x13dc; client.registers[4]=7;
        check(run_native(nt_iop_generated_run,client,ram,10,&linked).reason==NativeStop::import_request && !client.executed,
              "Explicitly unlinked client automatically rebound");
        check(control(8,0x13c8,36)==0,"Explicit native import relink failed");
        client.load_register=4; client.load_value=11;
        do { linked_stop=run_native(nt_iop_generated_run,client,ram,1,&linked); } while (linked_stop.reason==NativeStop::budget);
        check(client.registers[2]==16 && client.executed==4 && client.pc==0x5000,
              "Linked native transfer lost a pending argument or return destination");
        check(control(9,0x13c8,36)==0 && control(7,worker.address)==0,"Native import lifecycle did not release cleanly");
        check(control(10,worker.address)==0 && load_native_word(ram,worker.address+8)==0x10100 &&
              linked.resolve("worker",0x100,0)==0xa000,"Non-auto-link registration did not expose compiled exports");
        check(control(8,0x13c8,36)==0xffffffff,"Explicit linking selected a non-auto-link provider");
        client=initial(); client.pc=0x13dc;
        check(run_native(nt_iop_generated_run,client,ram,1,&linked).reason==NativeStop::import_request && !client.executed,
              "Non-auto-link registration bound an import");
        check(control(10,worker.address)==static_cast<std::uint32_t>(-214) &&
              control(10,0)==static_cast<std::uint32_t>(-214) && control(10,0xb800)==static_cast<std::uint32_t>(-214),
              "Non-auto-link registration accepted an active, null or unknown declaration");
        auto nonauto_query=initial(); nonauto_query.pc=0x1514; nonauto_query.registers[4]=worker.address;
        check(run_native(nt_iop_generated_run,nonauto_query,ram,1,&linked).reason==NativeStop::budget &&
              nonauto_query.registers[2]==worker.address+20,"Query hid a non-auto-link export table");
        check(control(7,worker.address)==0 && control(6,worker.address)==0 &&
              load_native_word(ram,worker.address+8)==0x100 && control(8,0x13c8,36)==0,
              "Normal registration did not clear the non-auto-link flag after release");
        client=initial(); client.pc=0x13dc; client.registers[4]=12;
        check(run_native(nt_iop_generated_run,client,ram,10,&linked).reason==NativeStop::unmapped_code &&
              client.registers[2]==17,"Re-enabled provider did not execute compiled code");
        check(control(9,0x13c8,36)==0 && control(7,worker.address)==0,"Non-auto-link lifecycle leaked bindings");
        store_native_word(ram,worker.address+20,0x4000);
        check(control(10,worker.address)==static_cast<std::uint32_t>(-214) &&
              !linked.resolve("worker",0x100,0) && load_native_word(ram,worker.address+8)==0x100,
              "Malformed non-auto-link declaration partially activated or changed modes");
        store_native_word(ram,worker.address+20,worker.functions[0]);
        auto short_registration=initial(); short_registration.pc=0x152c; short_registration.registers[4]=worker.address;
        check(run_native(nt_iop_generated_run,short_registration,std::span(ram).first(worker.address+24),1,&linked).reason==NativeStop::address_fault &&
              !short_registration.executed && !linked.resolve("worker",0x100,0),
              "Truncated non-auto-link declaration became visible");
        NativeLibraryServices transactional({worker,{0xa060,0x100,"other",{0x1360}}},
            {{0x13dc,0x13c8,"worker",0x103,0},{0x14a0,0x1480,"other",0x100,0}});
        check(transactional.invoke({6,{worker.address,0,0,0},NativeServiceKind::loadcore},ram).value==0,
              "Transactional fixture provider registration failed");
        check(transactional.invoke({9,{0x13c8,0x100,0,0},NativeServiceKind::loadcore},ram).value==0 &&
              transactional.invoke({8,{0x13c8,0x100,0,0},NativeServiceKind::loadcore},ram).value==0xffffffff,
              "Relink accepted a range with an unavailable provider");
        client=initial(); client.pc=0x13dc;
        check(run_native(nt_iop_generated_run,client,ram,1,&transactional).reason==NativeStop::import_request &&
              !client.executed && transactional.invoke({7,{worker.address,0,0,0},NativeServiceKind::loadcore},ram).value==0,
              "Failed multi-import relink committed partial bindings");
        NativeInterruptServices interrupts(0xf000);
        const auto intr_call=[&](unsigned ordinal,std::uint32_t a=0,std::uint32_t b=0,std::uint32_t c=0,std::uint32_t d=0) {
            const unsigned ordinals[]{4,5,6,7,8,9,17,18,23};
            const auto at=std::find(std::begin(ordinals),std::end(ordinals),ordinal);
            auto state=initial(); state.pc=0x1714+static_cast<std::uint32_t>(at-std::begin(ordinals))*8;
            state.registers[4]=a; state.registers[5]=b; state.registers[6]=c; state.registers[7]=d;
            const auto stop=run_native(nt_iop_generated_run,state,ram,1,&interrupts);
            check(stop.reason==NativeStop::budget && state.executed==1,"Compiled interrupt service did not return");
            return state.registers[2];
        };
        check(intr_call(9)==0,"Invocation fixture could not enable delivery");
        auto invocation=initial(); invocation.pc=0x17a0;
        invocation.registers[5]=0xc800; invocation.registers[6]=19; invocation.registers[7]=23;
        invocation.registers[28]=0x1234; invocation.registers[29]=0xe000;
        const auto invocation_saved=invocation;
        check(run_native(nt_iop_generated_run,invocation,ram,8,&interrupts).reason==NativeStop::budget &&
              invocation.pc==0x17c0 && !interrupts.delivery_enabled(),"Native invocation did not enter its discovered callback");
        auto competing=initial(); competing.pc=0x175c; competing.registers[4]=0x17c0;
        check(run_native(nt_iop_generated_run,competing,ram,1,&interrupts).reason==NativeStop::import_request && !competing.executed,
              "Concurrent invocation replaced the active continuation");
        NativeResult invocation_stop{};
        do { invocation_stop=run_native(nt_iop_generated_run,invocation,ram,1,&interrupts); }
        while (invocation_stop.reason==NativeStop::budget);
        check(invocation_stop.reason==NativeStop::unmapped_code && invocation.pc==0x5000 && invocation.registers[2]==42 &&
              load_native_word(ram,0xc800)==42 && interrupts.delivery_enabled() &&
              invocation.registers[28]==invocation_saved.registers[28] && invocation.registers[29]==invocation_saved.registers[29] &&
              invocation.native_call_owner==invocation_saved.native_call_owner,"Native invocation lost arguments, result or caller state");
        auto bad_invocation=initial(); bad_invocation.pc=0x175c; bad_invocation.registers[4]=0x4000;
        check(run_native(nt_iop_generated_run,bad_invocation,ram,1,&interrupts).reason==NativeStop::unmapped_code &&
              !bad_invocation.executed && interrupts.delivery_enabled(),"Unknown callback changed native delivery state");
        check(intr_call(8)==0,"Invocation fixture could not restore disabled delivery");
        auto registration=initial(); registration.pc=0x1b00;
        check(run_native(nt_iop_generated_run,registration,ram,32,&interrupts).reason==NativeStop::unmapped_code &&
              registration.pc==0x5000 && registration.executed==11 && registration.registers[2]==0,
              "Registration did not discover and bind its compiled callback");
        check(intr_call(4,3,2,0x1a00,0xc000)==static_cast<std::uint32_t>(-104) && intr_call(6,3)==0 && intr_call(9)==0,
              "Native handler duplicate or enable contract differs");
        check(intr_call(17,0xc008)==0 && load_native_word(ram,0xc008)==1 &&
              intr_call(17,0xc00c)==static_cast<std::uint32_t>(-102) && load_native_word(ram,0xc00c)==0,
              "Nested interrupt suspension lost saved mask state");
        interrupts.post(3);
        auto foreground=initial(); foreground.pc=0x5000; foreground.hi=123; foreground.lo=456;
        foreground.load_register=12; foreground.load_value=789; foreground.registers[16]=555;
        const auto saved_foreground=foreground;
        check(interrupts.run(nt_iop_generated_run,foreground,ram,10,interrupts).reason==NativeStop::unmapped_code && !foreground.executed,
              "Masked event executed a handler");
        check(intr_call(18,1)==0,"Native interrupt resume failed");
        check(interrupts.run(nt_iop_generated_run,foreground,ram,0,interrupts).reason==NativeStop::budget && !foreground.executed,
              "Zero-budget event dispatch changed continuation");
        store_native_word(ram,0xc000,0);
        check(interrupts.run(nt_iop_generated_run,foreground,ram,1,interrupts).reason==NativeStop::budget && foreground.pc==0x1a04,
              "Native callback did not suspend at a compiled budget boundary");
        check(intr_call(23)==1 && intr_call(5,3)==static_cast<std::uint32_t>(-100),"Interrupt context or handler release restriction differs");
        NativeResult irq_stop{};
        do { irq_stop=interrupts.run(nt_iop_generated_run,foreground,ram,1,interrupts); } while (irq_stop.reason==NativeStop::budget);
        check(irq_stop.reason==NativeStop::unmapped_code && foreground.pc==0x5000 && foreground.executed==13 &&
              foreground.registers==saved_foreground.registers && foreground.hi==123 && foreground.lo==456 &&
              foreground.load_register==12 && foreground.load_value==789 && load_native_word(ram,0xc000)==1 &&
              load_native_word(ram,0xc004)==1 && intr_call(23)==0,"Native callback lost context, payload or foreground state");
        interrupts.post(3);
        foreground=initial(); foreground.pc=0x1014; foreground.delay_slot=true; foreground.branch_target=0x5000;
        check(interrupts.run(nt_iop_generated_run,foreground,ram,32,interrupts).reason==NativeStop::unmapped_code &&
              foreground.pc==0x5000 && foreground.executed==14 && foreground.registers[10]==1 && load_native_word(ram,0xc000)==2,
              "Native event interrupted an outstanding delay slot");
        check(intr_call(7,3,0xc010)==0 && load_native_word(ram,0xc010)==3,"DisableIntr lost its restore token");
        interrupts.post(3); foreground=initial(); foreground.pc=0x5000;
        check(interrupts.run(nt_iop_generated_run,foreground,ram,32,interrupts).reason==NativeStop::unmapped_code && !foreground.executed,
              "Disabled cause was delivered");
        check(intr_call(6,3)==0,"Cause re-enable failed");
        check(interrupts.run(nt_iop_generated_run,foreground,ram,32,interrupts).reason==NativeStop::unmapped_code &&
              foreground.executed==13 && load_native_word(ram,0xc000)==3,"Masked pending cause was lost");
        auto bad_suspend=initial(); bad_suspend.pc=0x1744; bad_suspend.registers[4]=0x1000;
        check(run_native(nt_iop_generated_run,bad_suspend,ram,1,&interrupts).reason==NativeStop::code_write && !bad_suspend.executed,
              "Interrupt suspension overwrote converted code");
        check(intr_call(4,4,0,0x1a40)==0 && intr_call(6,4)==0,"Zero-return callback registration failed");
        interrupts.post(4); foreground=initial(); foreground.pc=0x5000;
        check(interrupts.run(nt_iop_generated_run,foreground,ram,32,interrupts).reason==NativeStop::unmapped_code && foreground.executed==3,
              "Zero-return handler did not execute");
        interrupts.post(4); foreground=initial(); foreground.pc=0x5000;
        check(interrupts.run(nt_iop_generated_run,foreground,ram,32,interrupts).reason==NativeStop::unmapped_code && !foreground.executed,
              "Zero-return handler did not leave its cause disabled");
        auto unknown_handler=initial(); unknown_handler.pc=0x1714; unknown_handler.registers[4]=5; unknown_handler.registers[6]=0x4000;
        const auto rejected_handler=run_native(nt_iop_generated_run,unknown_handler,ram,1,&interrupts);
        check(rejected_handler.reason==NativeStop::unmapped_code && rejected_handler.address==0x4000 && !unknown_handler.executed,
              "Uncompiled handler was registered");
        registration=initial(); registration.pc=0x1b40;
        check(run_native(nt_iop_generated_run,registration,ram,32,&interrupts).reason==NativeStop::unmapped_code &&
              registration.executed==11 && registration.registers[2]==0 && intr_call(6,7)==0,
              "Cross-module callback was not discovered and registered");
        store_native_word(ram,0xc014,40);
        interrupts.post(7); foreground=initial(); foreground.pc=0x5000;
        check(interrupts.run(nt_iop_generated_run,foreground,ram,32,interrupts).reason==NativeStop::unmapped_code &&
              foreground.pc==0x5000 && foreground.executed==7 && load_native_word(ram,0xc014)==41,
              "Cross-module native callback did not execute and restore its caller");
        check(intr_call(4,6,2,0x1a50)==0 && intr_call(6,6)==0,"Trapping callback registration failed");
        interrupts.post(6); foreground=initial(); foreground.pc=0x5000;
        const auto trapped_irq=interrupts.run(nt_iop_generated_run,foreground,ram,32,interrupts);
        check(trapped_irq.reason==NativeStop::guest_break && trapped_irq.trap_code==2 && foreground.pc==0x1a50 && !foreground.executed,
              "Callback failure was hidden or foreground execution resumed");
        // Compiled event calls and a compiled interrupt handler share native objects.
        NativeEventServices events;
        NativeInterruptServices event_interrupts(0xf000);
        NativeServiceRouter event_services({&events,&event_interrupts});
        const auto event_call=[&](unsigned ordinal,std::uint32_t a=0,std::uint32_t b=0,std::uint32_t c=0,std::uint32_t d=0) {
            const unsigned ordinals[]{4,5,6,7,8,9,10,11,13,14};
            const auto at=std::find(std::begin(ordinals),std::end(ordinals),ordinal);
            auto state=initial(); state.pc=0x1814+static_cast<std::uint32_t>(at-std::begin(ordinals))*8;
            state.registers[4]=a; state.registers[5]=b; state.registers[6]=c; state.registers[7]=d;
            const auto stop=run_native(nt_iop_generated_run,state,ram,1,&event_services);
            return std::pair{stop,state};
        };
        store_native_word(ram,0xc100,2); store_native_word(ram,0xc104,123); store_native_word(ram,0xc108,3);
        const auto created=event_call(4,0xc100);
        const auto event_id=created.second.registers[2];
        check(created.first.reason==NativeStop::budget && event_id>0 && event_id<0x80000000U,"Native event creation failed");
        check(event_call(11,event_id,4,0,0xc140).second.registers[2]==static_cast<std::uint32_t>(-421),"Unmet event poll succeeded");
        check(event_call(10,event_id,3,0,0xc140).second.registers[2]==0 && load_native_word(ram,0xc140)==3,
              "Satisfied native event wait did not return the full pattern");
        check(event_call(8,event_id,2).second.registers[2]==0 && event_call(11,event_id,3,1,0xc140).second.registers[2]==0 &&
              load_native_word(ram,0xc140)==2,"ClearEventFlag did not apply its keep mask");
        check(event_call(11,event_id,2,0x10,0x1000).first.reason==NativeStop::code_write &&
              event_call(11,event_id,2,0,0xc140).second.registers[2]==0,"Rejected event output consumed bits");
        check(event_call(11,event_id,2,0x10,0xc140).second.registers[2]==0 && load_native_word(ram,0xc140)==2,
              "Event clear-on-success lost result bits");
        const auto waiting=event_call(10,event_id,8,0,0xc140);
        check(waiting.first.reason==NativeStop::import_request && !waiting.second.executed && waiting.second.pc==0x1844,
              "Unsatisfied native wait falsely retired without a scheduler");
        check(event_call(7,event_id,8).second.registers[2]==static_cast<std::uint32_t>(-100),
              "Interrupt-only event call was accepted in foreground context");
        check(event_interrupts.invoke({4,{8,2,0x1c00,event_id},NativeServiceKind::intrman},ram).value==0 &&
              event_interrupts.invoke({6,{8,0,0,0},NativeServiceKind::intrman},ram).value==0 &&
              event_interrupts.invoke({9,{},NativeServiceKind::intrman},ram).value==0,"Event interrupt setup failed");
        event_interrupts.post(8); foreground=initial(); foreground.pc=0x5000;
        check(event_interrupts.run(nt_iop_generated_run,foreground,ram,32,event_services).reason==NativeStop::unmapped_code &&
              foreground.executed==9 && event_call(10,event_id,8,0x10,0xc140).second.registers[2]==0 &&
              load_native_word(ram,0xc140)==8,"Compiled interrupt did not signal the native event");
        store_native_word(ram,0xc154,0xfeed);
        check(event_call(13,event_id,0xc140).second.registers[2]==0 && load_native_word(ram,0xc140)==2 &&
              load_native_word(ram,0xc144)==123 && load_native_word(ram,0xc148)==3 &&
              load_native_word(ram,0xc14c)==0 && load_native_word(ram,0xc150)==0 && load_native_word(ram,0xc154)==0xfeed,
              "Native event status lost initial state or overwrote reserved fields");
        check(event_call(11,event_id,0).second.registers[2]==static_cast<std::uint32_t>(-423) &&
              event_call(11,event_id,1,2).second.registers[2]==static_cast<std::uint32_t>(-405),"Event pattern/mode validation differs");
        check(event_call(5,event_id).second.registers[2]==0 && event_call(6,event_id,1).second.registers[2]==static_cast<std::uint32_t>(-409) &&
              event_call(4,0xc100).second.registers[2]!=event_id,"Deleted event handle remained usable or was immediately recycled");
        const auto thread_status=[&](NativeServices& provider,std::uint32_t id,std::uint32_t output=0xc380) {
            auto query=initial(); query.pc=0x19ac; query.registers[4]=id; query.registers[5]=output;
            const auto stop=run_native(nt_iop_generated_run,query,ram,1,&provider);
            return std::pair{stop,query.registers[2]};
        };
        NativeInterruptServices scheduler_interrupts(0xf000);
        NativeThreadServices threads(scheduler_interrupts);
        NativeServiceRouter scheduler_services({&threads,&scheduler_interrupts});
        auto sleeper=initial(); sleeper.pc=0x1d00; sleeper.registers[31]=0xfffffffc;
        sleeper.hi=123; sleeper.lo=456; sleeper.registers[18]=789;
        const auto sleeper_id=threads.admit(sleeper);
        check(threads.run(nt_iop_generated_run,ram,4,scheduler_services).reason==NativeStop::budget &&
              threads.state(sleeper_id).pc==0x1d0c && threads.state(sleeper_id).executed==4,
              "Sleep did not retire exactly once and retain its continuation");
        check(threads.run(nt_iop_generated_run,ram,32,scheduler_services).reason==NativeStop::thread_wait && threads.executed()==4,
              "Idle scheduler consumed budget or fabricated progress");
        check(thread_status(threads,sleeper_id).second==0 && load_native_word(ram,0xc388)==4 &&
              load_native_word(ram,0xc38c)==0x1d00 && load_native_word(ram,0xc3a4)==1 && load_native_word(ram,0xc3a8)==0,
              "Compiled thread status lost sleeping state or its original entry");
        check(scheduler_interrupts.invoke({4,{9,2,0x1d80,sleeper_id},NativeServiceKind::intrman},ram).value==0 &&
              scheduler_interrupts.invoke({6,{9,0,0,0},NativeServiceKind::intrman},ram).value==0 &&
              scheduler_interrupts.invoke({9,{},NativeServiceKind::intrman},ram).value==0,"Scheduler interrupt setup failed");
        scheduler_interrupts.post(9);
        NativeResult scheduled{};
        for (unsigned slices=0;slices<32;++slices) {
            scheduled=threads.run(nt_iop_generated_run,ram,1,scheduler_services);
            if (scheduled.reason!=NativeStop::budget) break;
        }
        check(scheduled.reason==NativeStop::unmapped_code && scheduled.pc==0xfffffffc &&
              threads.state(sleeper_id).registers[2]==43 && threads.state(sleeper_id).hi==123 &&
              threads.state(sleeper_id).lo==456 && threads.state(sleeper_id).registers[18]==789 && threads.executed()==16,
              "Interrupt-side wakeup lost a sliced callback, thread context or operation accounting");
        check(threads.invoke({26,{sleeper_id,0,0,0},NativeServiceKind::thbase,0,true},ram).value==static_cast<std::uint32_t>(-413),
              "Wakeup revived a finished thread");
        NativeInterruptServices pair_interrupts(0xf000);
        NativeThreadServices pair_threads(pair_interrupts);
        NativeServiceRouter pair_services({&pair_threads,&pair_interrupts});
        const auto first_id=pair_threads.admit(sleeper);
        auto waker=initial(); waker.pc=0x1dc0; waker.registers[31]=0xfffffffc; waker.registers[4]=first_id;
        const auto second_id=pair_threads.admit(waker);
        check(pair_threads.run(nt_iop_generated_run,ram,64,pair_services).reason==NativeStop::unmapped_code &&
              pair_threads.state(second_id).registers[2]==99 && pair_threads.state(first_id).pc==0x1d0c,
              "Sleeping thread did not yield to the second compiled continuation");
        check(pair_threads.run(nt_iop_generated_run,ram,64,pair_services).reason==NativeStop::unmapped_code &&
              pair_threads.state(first_id).registers[2]==43 && pair_threads.executed()==16,
              "Foreground wakeup did not resume the sleeping continuation");
        for (const auto slice:{1U,64U}) {
            NativeInterruptServices worker_interrupts(0xf000);
            NativeThreadServices workers(worker_interrupts);
            NativeServiceRouter worker_services({&workers,&worker_interrupts});
            const auto sleeping=workers.admit(sleeper);
            auto waking=waker; waking.registers[4]=sleeping;
            const auto waking_id=workers.admit(waking);
            NativeResult result{NativeStop::budget,0};
            for (unsigned attempt=0;attempt<32 && result.reason==NativeStop::budget;++attempt)
                result=workers.run(nt_iop_generated_run,ram,slice,worker_services,
                    NativeThreadServices::CompletionPolicy::continue_workers);
            check(result.reason==NativeStop::thread_wait && workers.executed()==16 &&
                  workers.state(waking_id).registers[2]==99 && workers.state(sleeping).registers[2]==43,
                  "Persistent worker session stopped at a completed continuation or lost its budget");
            auto unknown=initial(); unknown.pc=0x1914; unknown.registers[31]=0x1fffc0;
            workers.admit(unknown);
            check(workers.run(nt_iop_generated_run,ram,64,worker_services,
                  NativeThreadServices::CompletionPolicy::continue_workers).reason==NativeStop::unmapped_code,
                  "Persistent worker session swallowed an unknown code target");
        }
        NativeInterruptServices queued_interrupts(0xf000);
        NativeThreadServices queued_threads(queued_interrupts);
        NativeServiceRouter queued_services({&queued_threads,&queued_interrupts});
        const auto queued_id=queued_threads.admit(sleeper);
        const NativeServiceRequest early_wake{26,{queued_id,0,0,0},NativeServiceKind::thbase,0,true};
        check(queued_threads.invoke(early_wake,ram).value==0 && queued_threads.invoke(early_wake,ram).value==0 &&
              queued_threads.invoke({28,{queued_id,0,0,0},NativeServiceKind::thbase,0,true},ram).value==2 &&
              queued_threads.invoke(early_wake,ram).value==0,"Queued wakeups were not counted or cancelled");
        check(queued_threads.run(nt_iop_generated_run,ram,32,queued_services).reason==NativeStop::unmapped_code &&
              queued_threads.state(queued_id).registers[2]==43 && queued_threads.executed()==8,
              "Early wakeup did not bypass one sleep without switching contexts");
        auto identity=initial(); identity.pc=0x1914; identity.registers[31]=0xfffffffc;
        const auto identity_id=queued_threads.admit(identity);
        check(queued_threads.run(nt_iop_generated_run,ram,2,queued_services).reason==NativeStop::unmapped_code &&
              queued_threads.state(identity_id).registers[2]==identity_id,"Compiled GetThreadId did not identify its continuation");
        NativeInterruptServices wait_interrupts(0xf000);
        NativeThreadServices wait_threads(wait_interrupts);
        NativeEventServices wait_events(&wait_threads);
        NativeServiceRouter wait_services({&wait_threads,&wait_interrupts,&wait_events});
        store_native_word(ram,0xc300,2); store_native_word(ram,0xc304,0); store_native_word(ram,0xc308,0);
        const auto wait_event=wait_events.invoke({4,{0xc300,0,0,0},NativeServiceKind::thevent},ram).value;
        const auto waiting_state=[&](std::uint32_t event,std::uint32_t mode,std::uint32_t output) {
            auto state=initial(); state.pc=0x1844; state.registers[31]=0xfffffffc;
            state.registers[4]=event; state.registers[5]=8; state.registers[6]=mode; state.registers[7]=output;
            return state;
        };
        const auto waiter_id=wait_threads.admit(waiting_state(wait_event,0x10,0xc320));
        check(wait_threads.run(nt_iop_generated_run,ram,32,wait_services).reason==NativeStop::thread_wait &&
              wait_threads.state(waiter_id).executed==1 && wait_threads.state(waiter_id).pc==0xfffffffc,
              "Event wait did not retire and suspend its compiled continuation");
        check(thread_status(wait_threads,waiter_id,0x8000c380).second==0 && load_native_word(ram,0xc388)==4 &&
              load_native_word(ram,0xc3a4)==4 && load_native_word(ram,0xc3a8)==wait_event,
              "Thread status did not report its native event wait handle");
        check(wait_events.invoke({13,{wait_event,0xc340,0,0},NativeServiceKind::thevent},ram).value==0 &&
              load_native_word(ram,0xc350)==1,"Event status did not count the suspended thread");
        check(wait_interrupts.invoke({4,{10,2,0x1c00,wait_event},NativeServiceKind::intrman},ram).value==0 &&
              wait_interrupts.invoke({6,{10,0,0,0},NativeServiceKind::intrman},ram).value==0 &&
              wait_interrupts.invoke({9,{},NativeServiceKind::intrman},ram).value==0,"Event wait interrupt setup failed");
        wait_interrupts.post(10);
        for (unsigned slice=0;slice<32;++slice) {
            scheduled=wait_threads.run(nt_iop_generated_run,ram,1,wait_services);
            if (scheduled.reason!=NativeStop::budget) break;
        }
        check(scheduled.reason==NativeStop::unmapped_code && wait_threads.state(waiter_id).registers[2]==0 &&
              load_native_word(ram,0xc320)==8 && wait_threads.executed()==10,
              "Interrupt signalling failed to complete a suspended event wait");
        check(thread_status(wait_threads,waiter_id).second==0 && load_native_word(ram,0xc388)==0x10 &&
              load_native_word(ram,0xc3a4)==0 && load_native_word(ram,0xc3a8)==0,
              "Completed thread retained stale event wait metadata");
        const auto first_waiter=wait_threads.admit(waiting_state(wait_event,0x10,0xc324));
        const auto second_waiter=wait_threads.admit(waiting_state(wait_event,0,0xc328));
        check(wait_threads.run(nt_iop_generated_run,ram,32,wait_services).reason==NativeStop::thread_wait,
              "Multiple event waiters did not suspend");
        auto signal=initial(); signal.pc=0x1824; signal.registers[31]=0xfffffffc;
        signal.registers[4]=wait_event; signal.registers[5]=8;
        wait_threads.admit(signal);
        check(wait_threads.run(nt_iop_generated_run,ram,32,wait_services).reason==NativeStop::unmapped_code &&
              wait_threads.run(nt_iop_generated_run,ram,32,wait_services).reason==NativeStop::unmapped_code &&
              wait_threads.state(first_waiter).registers[2]==0 && load_native_word(ram,0xc324)==8 &&
              wait_threads.run(nt_iop_generated_run,ram,32,wait_services).reason==NativeStop::thread_wait,
              "FIFO clear-on-success incorrectly woke a later waiter");
        auto remove=signal; remove.pc=0x181c;
        wait_threads.admit(remove);
        check(wait_threads.run(nt_iop_generated_run,ram,32,wait_services).reason==NativeStop::unmapped_code &&
              wait_threads.run(nt_iop_generated_run,ram,32,wait_services).reason==NativeStop::unmapped_code &&
              wait_threads.state(second_waiter).registers[2]==static_cast<std::uint32_t>(-425),
              "Event deletion did not release a queued wait with the deletion result");
        store_native_word(ram,0xc300,0);
        const auto single_event=wait_events.invoke({4,{0xc300,0,0,0},NativeServiceKind::thevent},ram).value;
        wait_threads.admit(waiting_state(single_event,0,0xc330));
        const auto rejected_waiter=wait_threads.admit(waiting_state(single_event,0,0xc334));
        check(wait_threads.run(nt_iop_generated_run,ram,32,wait_services).reason==NativeStop::unmapped_code &&
              wait_threads.state(rejected_waiter).registers[2]==static_cast<std::uint32_t>(-422),
              "Single-waiter event accepted a second suspended thread");
        const auto bad_waiter=wait_threads.admit(waiting_state(single_event+100,0,0x1000));
        check(wait_threads.run(nt_iop_generated_run,ram,32,wait_services).reason==NativeStop::unmapped_code &&
              wait_threads.state(bad_waiter).registers[2]==static_cast<std::uint32_t>(-409),"Unknown event queued a waiter");
        {
            NativeInterruptServices priority_interrupts(0xf000);
            NativeThreadServices priority_threads(priority_interrupts);
            auto controller=initial(); controller.pc=0x19a4; controller.registers[31]=0xfffffffc;
            controller.registers[4]=0; controller.registers[5]=20;
            const auto controller_id=priority_threads.admit(controller);
            auto work=initial(); work.pc=0xa000; work.registers[31]=0xfffffffc; work.registers[4]=10;
            const auto first=priority_threads.admit(work); work.registers[4]=20;
            const auto second=priority_threads.admit(work);
            check(priority_threads.run(nt_iop_generated_run,ram,32,priority_threads).reason==NativeStop::unmapped_code &&
                  priority_threads.state(first).registers[2]==15 && priority_threads.state(second).executed==0 &&
                  priority_threads.state(controller_id).executed==1,
                  "Lowering the current priority did not dispatch the first higher-priority worker");
            check(priority_threads.run(nt_iop_generated_run,ram,32,priority_threads).reason==NativeStop::unmapped_code &&
                  priority_threads.state(second).registers[2]==25 &&
                  priority_threads.run(nt_iop_generated_run,ram,32,priority_threads).reason==NativeStop::unmapped_code &&
                  priority_threads.state(controller_id).registers[2]==0,
                  "Priority dispatch lost FIFO order or the changing caller's continuation");
            controller.registers[4]=first; controller.registers[5]=1;
            const auto dormant=priority_threads.admit(controller);
            check(priority_threads.run(nt_iop_generated_run,ram,32,priority_threads).reason==NativeStop::unmapped_code &&
                  priority_threads.state(dormant).registers[2]==static_cast<std::uint32_t>(-413),
                  "Changing a finished thread's priority was accepted");
            controller.registers[4]=0; controller.registers[5]=127;
            const auto invalid=priority_threads.admit(controller);
            check(priority_threads.run(nt_iop_generated_run,ram,32,priority_threads).reason==NativeStop::unmapped_code &&
                  priority_threads.state(invalid).registers[2]==static_cast<std::uint32_t>(-403),
                  "Invalid priority did not return the public error");
            // Raising a different ready worker must yield at the service boundary.
            controller.registers[4]=0; controller.registers[5]=0;
            auto promoter=controller;
            work.registers[4]=30;
            const auto promoted=priority_threads.admit(work);
            promoter.registers[4]=promoted; promoter.registers[5]=1;
            const auto promoter_id=priority_threads.admit(promoter);
            priority_threads.invoke({14,{promoted,20,0,0},NativeServiceKind::thbase},ram);
            check(priority_threads.run(nt_iop_generated_run,ram,32,priority_threads).reason==NativeStop::unmapped_code &&
                  priority_threads.state(promoted).registers[2]==35 && priority_threads.state(promoter_id).executed==1 &&
                  priority_threads.run(nt_iop_generated_run,ram,32,priority_threads).reason==NativeStop::unmapped_code,
                  "Raising another ready worker did not reschedule the compiled caller");
            const auto unchanged=priority_threads.invoke({14,{0,1,0,0},NativeServiceKind::thbase,0,true},ram);
            check(unchanged.status==NativeServiceStatus::returned && unchanged.value==static_cast<std::uint32_t>(-100),
                  "Foreground priority operation accepted interrupt context");
        }
        {
            NativeInterruptServices status_interrupts(0xf000);
            NativeThreadServices status_threads(status_interrupts);
            auto query=initial(); query.pc=0x19ac; query.registers[4]=0; query.registers[5]=0xc380;
            query.registers[31]=0xfffffffc; query.registers[28]=0x1234;
            const auto id=status_threads.admit(query);
            check(thread_status(status_threads,id).second==0 && load_native_word(ram,0xc388)==2,
                  "Thread status did not distinguish a ready continuation");
            check(status_threads.run(nt_iop_generated_run,ram,32,status_threads).reason==NativeStop::unmapped_code &&
                  load_native_word(ram,0xc388)==1 && load_native_word(ram,0xc398)==0x1234 &&
                  load_native_word(ram,0xc39c)==8 && load_native_word(ram,0xc3a0)==8 &&
                  std::all_of(ram.begin()+0xc3b0,ram.begin()+0xc3c4,[](auto byte){return byte==0;}),
                  "Self status lost running state, priorities, GP or reserved-field initialization");
            const auto snapshot=ram;
            check(thread_status(status_threads,id,0x1000).first.reason==NativeStop::code_write &&
                  thread_status(status_threads,id,0xffc0).first.reason==NativeStop::address_fault &&
                  thread_status(status_threads,id+100).second==static_cast<std::uint32_t>(-407) && ram==snapshot,
                  "Rejected thread-status query partially wrote RAM");
        }
        {
            auto now=std::chrono::steady_clock::time_point{};
            NativeInterruptServices delay_interrupts(0xf000);
            NativeThreadServices delayed(delay_interrupts,nullptr,[&] { return now; });
            delay_interrupts.invoke({9,{},NativeServiceKind::intrman},ram);
            auto worker=initial(); worker.pc=0x19b4; worker.registers[4]=250; worker.registers[31]=0xfffffffc;
            const auto id=delayed.admit(worker);
            check(delayed.run(nt_iop_generated_run,ram,32,delayed).reason==NativeStop::thread_wait &&
                  delayed.state(id).executed==1 && delayed.delay()==std::chrono::microseconds(250) &&
                  thread_status(delayed,id).second==0 && load_native_word(ram,0xc388)==4 &&
                  load_native_word(ram,0xc3a4)==2 && load_native_word(ram,0xc3a8)==250,
                  "Compiled DelayThread did not suspend with a native deadline and public wait status");
            delayed.invoke({25,{id,0,0,0},NativeServiceKind::thbase},ram);
            now+=std::chrono::microseconds(249);
            check(delayed.run(nt_iop_generated_run,ram,32,delayed).reason==NativeStop::thread_wait &&
                  delayed.state(id).executed==1 && delayed.delay()==std::chrono::microseconds(1),
                  "Wakeup or an early clock sample completed a delay prematurely");
            now+=std::chrono::microseconds(1);
            check(delayed.run(nt_iop_generated_run,ram,0,delayed).reason==NativeStop::budget &&
                  thread_status(delayed,id).second==0 && load_native_word(ram,0xc388)==4,
                  "Zero-budget scheduler advanced a timed wait");
            delay_interrupts.invoke({8,{},NativeServiceKind::intrman},ram);
            check(delayed.run(nt_iop_generated_run,ram,32,delayed).reason==NativeStop::thread_wait,
                  "Masked interrupt delivery completed a timed wait");
            delay_interrupts.invoke({9,{},NativeServiceKind::intrman},ram);
            check(delayed.run(nt_iop_generated_run,ram,32,delayed).reason==NativeStop::unmapped_code &&
                  delayed.state(id).registers[2]==0 && !delayed.delay() &&
                  thread_status(delayed,id).second==0 && load_native_word(ram,0xc3a4)==0 && load_native_word(ram,0xc3a8)==0 &&
                  load_native_word(ram,0xc3ac)==1,
                  "Expired delay lost its caller, retained wait metadata or consumed an unrelated wakeup");
            worker.registers[4]=0;
            const auto minimum_id=delayed.admit(worker);
            check(delayed.run(nt_iop_generated_run,ram,32,delayed).reason==NativeStop::thread_wait &&
                  delayed.delay()==std::chrono::microseconds(100),"Zero delay did not apply the minimum native wait");
            now+=std::chrono::microseconds(100);
            check(delayed.run(nt_iop_generated_run,ram,32,delayed).reason==NativeStop::unmapped_code &&
                  delayed.state(minimum_id).executed==1,"Minimum delay failed to resume exactly once");
        }
        {
            NativeInterruptServices sema_interrupts(0xf000);
            NativeThreadServices sema_threads(sema_interrupts);
            NativeSemaphoreServices semaphores(&sema_threads);
            NativeServiceRouter sema_services({&semaphores,&sema_threads});
            const auto sema_call=[&](unsigned op,std::uint32_t a,std::uint32_t b=0,bool irq=false) {
                auto state=initial(); state.pc=op<=9 ? 0x2714+(op-4)*8 : 0x2744+(op-11)*8;
                state.registers[4]=a; state.registers[5]=b; state.interrupt_depth=irq ? 1 : 0;
                const auto stop=run_native(nt_iop_generated_run,state,ram,1,&sema_services);
                return std::pair{stop,state.registers[2]};
            };
            store_native_word(ram,0xc200,1); store_native_word(ram,0xc204,123);
            store_native_word(ram,0xc208,0); store_native_word(ram,0xc20c,1);
            const auto semaphore=sema_call(4,0xc200).second;
            auto wait=initial(); wait.pc=0x2734; wait.registers[4]=semaphore; wait.registers[31]=0xfffffffc;
            const auto first=sema_threads.admit(wait),second=sema_threads.admit(wait);
            check(sema_threads.run(nt_iop_generated_run,ram,32,sema_services).reason==NativeStop::thread_wait &&
                  sema_threads.state(first).executed==1 && sema_threads.state(second).executed==1 &&
                  thread_status(sema_threads,second).second==0 && load_native_word(ram,0xc3a4)==3 && load_native_word(ram,0xc3a8)==semaphore,
                  "Compiled semaphore wait did not suspend workers with native wait metadata");
            store_native_word(ram,0xc258,0x12345678);
            check(sema_call(11,semaphore,0xc240).second==0 && load_native_word(ram,0xc244)==123 &&
                  load_native_word(ram,0xc254)==2 && load_native_word(ram,0xc258)==0x12345678 &&
                  sema_call(9,semaphore).second==static_cast<std::uint32_t>(-419),
                  "Semaphore status, reserved fields or empty polling differs");
            sema_threads.invoke({14,{second,1,0,0},NativeServiceKind::thbase},ram);
            check(sema_call(6,semaphore).second==0 && sema_threads.run(nt_iop_generated_run,ram,32,sema_services).reason==NativeStop::unmapped_code &&
                  sema_threads.state(second).registers[2]==0 && thread_status(sema_threads,first).second==0 &&
                  load_native_word(ram,0xc388)==4,"Priority semaphore ignored a waiting worker's changed priority");
            check(sema_call(5,semaphore).second==0 && sema_threads.run(nt_iop_generated_run,ram,32,sema_services).reason==NativeStop::unmapped_code &&
                  sema_threads.state(first).registers[2]==static_cast<std::uint32_t>(-425) &&
                  sema_call(6,semaphore).second==static_cast<std::uint32_t>(-408),
                  "Semaphore deletion did not release its waiter or invalidate its handle");
            store_native_word(ram,0xc200,0);
            const auto fifo=sema_call(4,0xc200).second; wait.registers[4]=fifo;
            const auto fifo_first=sema_threads.admit(wait),fifo_second=sema_threads.admit(wait);
            check(sema_threads.run(nt_iop_generated_run,ram,32,sema_services).reason==NativeStop::thread_wait,
                  "FIFO semaphore workers did not suspend");
            sema_threads.invoke({14,{fifo_second,1,0,0},NativeServiceKind::thbase},ram);
            check(sema_call(7,fifo,0,true).second==0 && sema_threads.run(nt_iop_generated_run,ram,32,sema_services).reason==NativeStop::unmapped_code &&
                  sema_threads.state(fifo_first).registers[2]==0 && thread_status(sema_threads,fifo_second).second==0 && load_native_word(ram,0xc388)==4,
                  "Interrupt semaphore signal violated FIFO ordering");
            check(sema_call(6,fifo).second==0 && sema_threads.run(nt_iop_generated_run,ram,32,sema_services).reason==NativeStop::unmapped_code &&
                  sema_call(6,fifo).second==0 && sema_call(6,fifo).second==static_cast<std::uint32_t>(-420) &&
                  sema_call(9,fifo).second==0 && sema_call(9,fifo).second==static_cast<std::uint32_t>(-419),
                  "Semaphore count was lost, duplicated or allowed to overflow");
            const auto before_status=ram;
            check(sema_call(11,fifo,0x1000).first.reason==NativeStop::code_write &&
                  sema_call(11,fifo,0xfff0).first.reason==NativeStop::address_fault && ram==before_status &&
                  sema_call(7,fifo).second==static_cast<std::uint32_t>(-100),
                  "Semaphore status partially wrote an invalid buffer or accepted wrong interrupt context");
        }
        {
            NativeInterruptServices mbx_interrupts(0xf000);
            NativeThreadServices mbx_threads(mbx_interrupts);
            NativeMailboxServices mailboxes(&mbx_threads);
            NativeServiceRouter mbx_services({&mailboxes,&mbx_threads});
            const auto mbx_call=[&](unsigned op,std::uint32_t a,std::uint32_t b=0,bool irq=false) {
                auto state=initial(); state.pc=op<=9 ? 0x2794+(op-4)*8 : 0x27c4+(op-11)*8;
                state.registers[4]=a; state.registers[5]=b; state.interrupt_depth=irq ? 1 : 0;
                const auto stop=run_native(nt_iop_generated_run,state,ram,1,&mbx_services);
                return std::pair{stop,state.registers[2]};
            };
            store_native_word(ram,0xc200,5); store_native_word(ram,0xc204,42);
            const auto box=mbx_call(4,0xc200).second;
            store_native_word(ram,0xc264,9); store_native_word(ram,0xc274,2);
            check(mbx_call(6,box,0x8000c260).second==0 && mbx_call(6,box,0xc270).second==0 &&
                  load_native_word(ram,0xc260)==0xc270 && load_native_word(ram,0xc270)==0x8000c260,
                  "Native mailbox did not mirror circular priority-ordered message links");
            const auto queued=ram;
            check(mbx_call(6,box,0xa000c260).first.reason==NativeStop::import_request &&
                  mbx_call(9,0x1000,box).first.reason==NativeStop::code_write && ram==queued,
                  "Duplicate message or protected receive modified a queued mailbox");
            store_native_word(ram,0xc254,0x12345678);
            check(mbx_call(11,box,0xc240).second==0 && load_native_word(ram,0xc24c)==2 &&
                  load_native_word(ram,0xc250)==0xc270 && load_native_word(ram,0xc254)==0x12345678 &&
                  mbx_call(9,0xc280,box).second==0 && load_native_word(ram,0xc280)==0xc270 &&
                  load_native_word(ram,0xc260)==0x8000c260 && mbx_call(9,0xc280,box).second==0 &&
                  load_native_word(ram,0xc280)==0x8000c260 && mbx_call(9,0xc280,box).second==static_cast<std::uint32_t>(-424),
                  "Mailbox receive lost priority order, pointer alias, status or empty-poll semantics");
            auto receive=initial(); receive.pc=0x27b4; receive.registers[4]=0xc280; receive.registers[5]=box; receive.registers[31]=0xfffffffc;
            const auto first=mbx_threads.admit(receive); receive.registers[4]=0xc284;
            const auto second=mbx_threads.admit(receive);
            check(mbx_threads.run(nt_iop_generated_run,ram,32,mbx_services).reason==NativeStop::thread_wait &&
                  thread_status(mbx_threads,first).second==0 && load_native_word(ram,0xc3a4)==5 && load_native_word(ram,0xc3a8)==box,
                  "Mailbox receive did not suspend with native wait metadata");
            mbx_threads.invoke({14,{second,1,0,0},NativeServiceKind::thbase},ram);
            check(mbx_call(7,box,0xc260,true).second==0 && mbx_threads.run(nt_iop_generated_run,ram,32,mbx_services).reason==NativeStop::unmapped_code &&
                  mbx_threads.state(second).registers[2]==0 && load_native_word(ram,0xc284)==0xc260,
                  "Interrupt send did not deliver directly to the highest-priority waiter");
            check(mbx_call(5,box).second==0 && mbx_threads.run(nt_iop_generated_run,ram,32,mbx_services).reason==NativeStop::unmapped_code &&
                  mbx_threads.state(first).registers[2]==static_cast<std::uint32_t>(-425) &&
                  mbx_call(6,box,0xc260).second==static_cast<std::uint32_t>(-410),
                  "Mailbox deletion lost pending waiters or retained a live handle");
            store_native_word(ram,0xc200,0);
            const auto fifo=mbx_call(4,0xc200).second;
            check(mbx_call(6,fifo,0xc260).second==0 && mbx_call(6,fifo,0xc270).second==0 &&
                  mbx_call(9,0xc280,fifo).second==0 && load_native_word(ram,0xc280)==0xc260 &&
                  mbx_call(9,0xc280,fifo).second==0 && load_native_word(ram,0xc280)==0xc270 &&
                  mbx_call(7,fifo,0xc260).second==static_cast<std::uint32_t>(-100),
                  "FIFO mailbox reordered messages by priority or accepted incorrect interrupt context");
        }
        for (const auto entry : {0x19bcU,0x1a60U}) {
            NativeHeapServices stack_heap(0x10000,0xd000,0xe000);
            NativeInterruptServices stack_interrupts(0xf000);
            NativeThreadServices stack_threads(stack_interrupts,&stack_heap);
            NativeServiceRouter stack_services({&stack_threads,&stack_interrupts});
            store_native_word(ram,0xc400,0); store_native_word(ram,0xc404,0);
            store_native_word(ram,0xc408,entry); store_native_word(ram,0xc40c,0x200); store_native_word(ram,0xc410,1);
            auto create_stack=initial(); create_stack.pc=0x1944; create_stack.registers[4]=0xc400;
            check(run_native(nt_iop_generated_run,create_stack,ram,1,&stack_services).reason==NativeStop::budget,
                  "Stack-check worker creation failed");
            const auto id=create_stack.registers[2];
            auto start_stack=initial(); start_stack.pc=0x1954; start_stack.registers[4]=id;
            check(run_native(nt_iop_generated_run,start_stack,ram,1,&stack_services).reason==NativeStop::budget,
                  "Stack-check worker start failed");
            const auto result=stack_threads.run(nt_iop_generated_run,ram,32,stack_services);
            if (entry==0x19bc) check(result.reason==NativeStop::unmapped_code && stack_threads.state(id).registers[2]==0x1f0,
                                       "Stack query did not report the owned stack's remaining bytes");
            else check(result.reason==NativeStop::address_fault && result.address==0xde70 &&
                       stack_threads.state(id).pc==0x19bc && stack_threads.state(id).executed==3,
                       "Low-stack check retired or fabricated a successful result");
            auto no_stack=initial(); no_stack.pc=0x19bc;
            check(run_native(nt_iop_generated_run,no_stack,ram,1,&stack_services).reason==NativeStop::import_request && !no_stack.executed,
                  "Stack query inferred ownership for an unrelated continuation");
            no_stack.interrupt_depth=1;
            check(run_native(nt_iop_generated_run,no_stack,ram,1,&stack_services).reason==NativeStop::budget && no_stack.registers[2]==0,
                  "Interrupt-context stack query did not return the public zero result");
        }
        for (const auto kind : {NativeServiceKind::thevent,NativeServiceKind::thsemap,NativeServiceKind::thmsgbx}) {
            NativeInterruptServices terminate_interrupts(0xf000);
            NativeThreadServices terminate_threads(terminate_interrupts);
            NativeEventServices event_queue(&terminate_threads);
            NativeSemaphoreServices semaphore_queue(&terminate_threads);
            NativeMailboxServices mailbox_queue(&terminate_threads);
            NativeServices* queue=kind==NativeServiceKind::thevent ? static_cast<NativeServices*>(&event_queue) :
                kind==NativeServiceKind::thsemap ? static_cast<NativeServices*>(&semaphore_queue) : static_cast<NativeServices*>(&mailbox_queue);
            NativeServiceRouter services({&terminate_threads,queue});
            store_native_word(ram,0xc400,kind==NativeServiceKind::thevent ? 2 : 0);
            store_native_word(ram,0xc404,0); store_native_word(ram,0xc408,0); store_native_word(ram,0xc40c,1);
            const auto object=queue->invoke({4,{0xc400},kind},ram).value;
            auto waiter=initial(); waiter.registers[31]=0xfffffffc;
            waiter.pc=kind==NativeServiceKind::thevent ? 0x1844 : kind==NativeServiceKind::thsemap ? 0x2734 : 0x27b4;
            waiter.registers[4]=kind==NativeServiceKind::thmsgbx ? 0xc500 : object;
            waiter.registers[5]=kind==NativeServiceKind::thmsgbx ? object : 8;
            waiter.registers[6]=0; waiter.registers[7]=0xc500;
            const auto cancelled=terminate_threads.admit(waiter);
            if (kind==NativeServiceKind::thmsgbx) waiter.registers[4]=0xc504;
            else waiter.registers[7]=0xc504;
            const auto survivor=terminate_threads.admit(waiter);
            check(terminate_threads.run(nt_iop_generated_run,ram,32,services).reason==NativeStop::thread_wait,
                  "Termination fixture did not enter object waits");
            store_native_word(ram,0xc500,0x12345678);
            auto terminate=initial(); terminate.pc=0x19c4; terminate.registers[4]=cancelled;
            check(run_native(nt_iop_generated_run,terminate,ram,1,&services).reason==NativeStop::budget && terminate.registers[2]==0,
                  "Compiled termination did not detach its object wait");
            const auto signal=kind==NativeServiceKind::thmsgbx ? 0xc600U : 8U;
            store_native_word(ram,0xc600,0); store_native_word(ram,0xc604,0);
            check(queue->invoke({6,{object,signal},kind},ram).status==NativeServiceStatus::returned &&
                  terminate_threads.run(nt_iop_generated_run,ram,32,services).reason==NativeStop::unmapped_code &&
                  terminate_threads.state(survivor).registers[2]==0 && terminate_threads.state(cancelled).executed==1 &&
                  load_native_word(ram,0xc500)==0x12345678,"Signal resumed or wrote output for a terminated waiter");
            terminate=initial(); terminate.pc=0x19c4; terminate.registers[4]=cancelled;
            check(run_native(nt_iop_generated_run,terminate,ram,1,&services).reason==NativeStop::budget &&
                  terminate.registers[2]==std::uint32_t(-413),"Repeated termination did not report dormant status");
            check(terminate_threads.run(nt_iop_generated_run,ram,32,services).reason==NativeStop::thread_wait,
                  "Terminated continuation remained scheduled");
        }
        {
            NativeHeapServices owned(0x10000,0xd000,0xe000);
            NativeInterruptServices interrupts(0xf000);
            NativeThreadServices threads(interrupts,&owned);
            auto creator=initial(); creator.pc=0x1f60; creator.registers[29]=0xc900;
            check(run_native(nt_iop_generated_run,creator,ram,64,&threads).reason==NativeStop::unmapped_code &&
                  creator.pc==0x5000 && creator.registers[29]==0xc900 && creator.registers[2]<0x80000000,
                  "Stack-built thread descriptor failed to discover its compiled entry");
            auto start=initial(); start.pc=0x1954; start.registers[4]=creator.registers[2];
            check(run_native(nt_iop_generated_run,start,ram,1,&threads).reason==NativeStop::budget &&
                  threads.run(nt_iop_generated_run,ram,32,threads).reason==NativeStop::unmapped_code &&
                  threads.state(creator.registers[2]).registers[2]==73,
                  "Stack-discovered worker did not execute native code");
        }
        NativeHeapServices thread_heap(0x10000,0xd000,0xe000);
        NativeInterruptServices lifecycle_interrupts(0xf000);
        NativeThreadServices lifecycle(lifecycle_interrupts,&thread_heap);
        NativeServiceRouter lifecycle_services({&lifecycle,&lifecycle_interrupts});
        store_native_word(ram,0xc400,0x02200000); store_native_word(ram,0xc404,0x7654);
        store_native_word(ram,0xc408,0xa000); store_native_word(ram,0xc40c,0x131); store_native_word(ram,0xc410,1);
        auto creator=initial(); creator.pc=0x1944; creator.registers[31]=0xfffffffc;
        creator.registers[4]=0xc400; creator.registers[28]=0x1234;
        const auto creator_id=lifecycle.admit(creator);
        check(lifecycle.run(nt_iop_generated_run,ram,32,lifecycle_services).reason==NativeStop::unmapped_code,
              "Compiled CreateThread did not return");
        const auto worker_id=lifecycle.state(creator_id).registers[2];
        check(worker_id>0 && worker_id<0x80000000U && load_native_word(ram,0xc40c)==0x200 && ram[0xde00]==0xff &&
              thread_heap.invoke({8,{},NativeServiceKind::sysmem},ram).value==0xe00,
              "Native thread creation did not align, fill and reserve its stack");
        check(thread_status(lifecycle,worker_id).second==0 && load_native_word(ram,0xc380)==0x02200000 &&
              load_native_word(ram,0xc384)==0x7654 && load_native_word(ram,0xc388)==0x10 &&
              load_native_word(ram,0xc38c)==0xa000 && load_native_word(ram,0xc390)==0xde00 &&
              load_native_word(ram,0xc394)==0x200 && load_native_word(ram,0xc39c)==1,
              "Created-thread status lost descriptor metadata or stack ownership");
        auto prepare_termination=initial(); prepare_termination.pc=0x1954; prepare_termination.registers[4]=worker_id;
        check(run_native(nt_iop_generated_run,prepare_termination,ram,1,&lifecycle_services).reason==NativeStop::budget,
              "Termination fixture did not start its owned worker");
        auto terminate_worker=initial(); terminate_worker.pc=0x19c4; terminate_worker.registers[4]=worker_id;
        check(run_native(nt_iop_generated_run,terminate_worker,ram,1,&lifecycle_services).reason==NativeStop::budget &&
              terminate_worker.registers[2]==0 && thread_heap.invoke({8,{},NativeServiceKind::sysmem},ram).value==0xe00,
              "Terminating a ready worker released its reusable stack");
        auto starter=initial(); starter.pc=0x1954; starter.registers[31]=0xfffffffc;
        starter.registers[4]=worker_id; starter.registers[5]=42;
        const auto starter_id=lifecycle.admit(starter);
        check(lifecycle.run(nt_iop_generated_run,ram,32,lifecycle_services).reason==NativeStop::unmapped_code &&
              lifecycle.state(worker_id).registers[2]==47 && lifecycle.state(worker_id).registers[28]==0x1234 &&
              lifecycle.state(worker_id).registers[29]==0xdff0 && lifecycle.state(starter_id).executed==1,
              "StartThread did not dispatch its higher-priority compiled worker with argument, GP and stack");
        check(lifecycle.run(nt_iop_generated_run,ram,32,lifecycle_services).reason==NativeStop::unmapped_code &&
              lifecycle.state(starter_id).registers[2]==0,"StartThread caller did not resume after its worker");
        starter.registers[5]=7;
        lifecycle.admit(starter);
        check(lifecycle.run(nt_iop_generated_run,ram,32,lifecycle_services).reason==NativeStop::unmapped_code &&
              lifecycle.state(worker_id).registers[2]==12 &&
              lifecycle.run(nt_iop_generated_run,ram,32,lifecycle_services).reason==NativeStop::unmapped_code,
              "Dormant compiled thread could not be restarted with fresh arguments");
        auto deleter=starter; deleter.pc=0x194c;
        const auto deleter_id=lifecycle.admit(deleter);
        check(lifecycle.run(nt_iop_generated_run,ram,32,lifecycle_services).reason==NativeStop::unmapped_code &&
              lifecycle.state(deleter_id).registers[2]==0 &&
              std::all_of(ram.begin()+0xde00,ram.begin()+0xe000,[](auto byte){return byte==0;}) &&
              thread_heap.invoke({8,{},NativeServiceKind::sysmem},ram).value==0x1000,
              "DeleteThread did not clear and release its dormant worker stack");
        const auto stale_starter=lifecycle.admit(starter);
        check(lifecycle.run(nt_iop_generated_run,ram,32,lifecycle_services).reason==NativeStop::unmapped_code &&
              lifecycle.state(stale_starter).registers[2]==static_cast<std::uint32_t>(-407),"Deleted worker handle remained startable");
        store_native_word(ram,0xc408,0x4000);
        const auto invalid_creator=lifecycle.admit(creator);
        check(lifecycle.run(nt_iop_generated_run,ram,32,lifecycle_services).reason==NativeStop::unmapped_code &&
              !lifecycle.state(invalid_creator).executed && thread_heap.invoke({8,{},NativeServiceKind::sysmem},ram).value==0x1000,
              "Uncompiled thread entry allocated a stack or retired creation");
        for (const auto worker_entry : {0x1e00U,0x1e40U}) {
            NativeHeapServices exit_heap(0x10000,0xd000,0xe000);
            NativeInterruptServices exit_interrupts(0xf000);
            NativeThreadServices exit_threads(exit_interrupts,&exit_heap);
            NativeServiceRouter exit_services({&exit_threads,&exit_interrupts});
            store_native_word(ram,0xc408,worker_entry); store_native_word(ram,0xc40c,0x200);
            const auto exit_creator=exit_threads.admit(creator);
            check(exit_threads.run(nt_iop_generated_run,ram,32,exit_services).reason==NativeStop::unmapped_code,
                  "Exit fixture worker creation failed");
            const auto exit_worker=exit_threads.state(exit_creator).registers[2];
            auto exit_start=starter; exit_start.registers[4]=exit_worker;
            const auto exit_parent=exit_threads.admit(exit_start);
            check(exit_threads.run(nt_iop_generated_run,ram,1,exit_services).reason==NativeStop::budget &&
                  exit_threads.run(nt_iop_generated_run,ram,2,exit_services).reason==NativeStop::budget &&
                  exit_threads.state(exit_worker).executed==2,
                  "Exit fixture did not suspend immediately before the compiled exit import");
            const auto before_exit=exit_threads.executed();
            check(exit_threads.run(nt_iop_generated_run,ram,0,exit_services).reason==NativeStop::budget &&
                  exit_threads.executed()==before_exit && exit_threads.contains(exit_worker),
                  "Zero-budget thread exit changed lifecycle state");
            const auto stopped=exit_threads.run(nt_iop_generated_run,ram,1,exit_services);
            check(stopped.reason==NativeStop::unmapped_code && stopped.pc==0xfffffffc &&
                  exit_threads.executed()==before_exit+1,"Explicit thread exit returned into trailing guest code");
            if (worker_entry==0x1e40) {
                check(!exit_threads.contains(exit_worker) && exit_heap.invoke({8,{},NativeServiceKind::sysmem},ram).value==0x1000 &&
                      std::all_of(ram.begin()+0xde00,ram.begin()+0xe000,[](auto byte){return byte==0;}),
                      "ExitDeleteThread did not remove its handle and clear/release its stack");
            } else {
                check(exit_threads.contains(exit_worker) && exit_threads.state(exit_worker).pc==0xfffffffc &&
                      exit_heap.invoke({8,{},NativeServiceKind::sysmem},ram).value==0xe00,
                      "ExitThread did not retain a dormant worker and its stack");
                check(exit_threads.invoke({6,{exit_worker,0,0,0},NativeServiceKind::thbase},ram).status==NativeServiceStatus::returned,
                      "ExitThread worker could not be restarted");
                check(exit_threads.run(nt_iop_generated_run,ram,32,exit_services).reason==NativeStop::unmapped_code,
                      "Restarted explicit-exit worker did not terminate");
                check(exit_threads.invoke({5,{exit_worker,0,0,0},NativeServiceKind::thbase},ram).value==0 &&
                      exit_heap.invoke({8,{},NativeServiceKind::sysmem},ram).value==0x1000,
                      "Dormant explicit-exit worker could not release its stack");
            }
            check(exit_threads.run(nt_iop_generated_run,ram,32,exit_services).reason==NativeStop::unmapped_code &&
                  exit_threads.state(exit_parent).registers[2]==0,"Thread exit did not leave its caller runnable");
            auto interrupt_exit=initial(); interrupt_exit.pc=0x195c; interrupt_exit.interrupt_depth=1;
            check(run_native(nt_iop_generated_run,interrupt_exit,ram,1,&exit_threads).reason==NativeStop::budget &&
                  interrupt_exit.registers[2]==static_cast<std::uint32_t>(-100),"ExitThread accepted interrupt context");
        }
        NativeHeapServices args_heap(0x10000,0xd000,0xe000);
        NativeInterruptServices args_interrupts(0xf000);
        NativeThreadServices args_threads(args_interrupts,&args_heap);
        NativeServiceRouter args_services({&args_threads,&args_interrupts});
        store_native_word(ram,0xc408,0x1e80); store_native_word(ram,0xc40c,0x200);
        store_native_word(ram,0xa040,0x02200000); store_native_word(ram,0xa044,0);
        store_native_word(ram,0xa048,0x80001e80); store_native_word(ram,0xa04c,0x200); store_native_word(ram,0xa050,1);
        auto descriptor_creator=creator; descriptor_creator.pc=0x1f20;
        const auto args_creator=args_threads.admit(descriptor_creator);
        check(args_threads.run(nt_iop_generated_run,ram,32,args_services).reason==NativeStop::unmapped_code,
              "Argument-block worker creation failed");
        const auto args_worker=args_threads.state(args_creator).registers[2];
        const auto dormant=args_threads.invoke({23,{args_worker,0xcc00},NativeServiceKind::thbase,0,true},ram);
        check(dormant.status==NativeServiceStatus::returned && dormant.value==0 &&
              load_native_word(ram,0xcc08)==0x10 && load_native_word(ram,0xcc30)==0,
              "Interrupt-context dormant status did not expose its actual state");
        check(args_threads.invoke({23,{args_worker,0xcc00},NativeServiceKind::thbase},ram).value==static_cast<std::uint32_t>(-100),
              "Interrupt-only thread status accepted foreground context");
        auto args_start=initial(); args_start.pc=0x196c; args_start.registers[31]=0xfffffffc;
        args_start.registers[4]=args_worker; args_start.registers[5]=3; args_start.registers[6]=0xffff;
        auto bad_args=args_start;
        check(run_native(nt_iop_generated_run,bad_args,ram,1,&args_threads).reason==NativeStop::address_fault &&
              !bad_args.executed && args_threads.state(args_worker).executed==0,
              "Invalid argument source changed a dormant worker or retired StartThreadArgs");
        bad_args=args_start; bad_args.registers[5]=0xffffffff;
        check(run_native(nt_iop_generated_run,bad_args,ram,1,&args_threads).reason==NativeStop::import_request &&
              !bad_args.executed,"Oversized argument block underflowed the worker stack");
        ram[0xc500]=40; ram[0xc501]=7; ram[0xc502]=9;
        args_start.registers[6]=0x8000c500;
        const auto args_parent=args_threads.admit(args_start);
        check(args_threads.run(nt_iop_generated_run,ram,32,args_services).reason==NativeStop::unmapped_code &&
              args_threads.state(args_worker).registers[2]==43 && args_threads.state(args_worker).registers[4]==3 &&
              args_threads.state(args_worker).registers[5]==0xdff0 && args_threads.state(args_worker).registers[29]==0xdfe0 &&
              args_threads.state(args_worker).registers[30]==0xdfe0 && ram[0xdff0]==40 && ram[0xdff1]==7 && ram[0xdff2]==9,
              "StartThreadArgs did not copy its block or provide the native worker ABI");
        ram[0xc500]=99;
        check(ram[0xdff0]==40 && args_threads.run(nt_iop_generated_run,ram,32,args_services).reason==NativeStop::unmapped_code &&
              args_threads.state(args_parent).registers[2]==0,"Worker argument block retained the caller source or lost its caller");
        ram[0xdfee]=40; ram[0xdfef]=7; ram[0xdff0]=9;
        args_start.registers[6]=0xdfee;
        args_threads.admit(args_start);
        check(args_threads.run(nt_iop_generated_run,ram,32,args_services).reason==NativeStop::unmapped_code &&
              args_threads.state(args_worker).registers[2]==43 && ram[0xdff0]==40 && ram[0xdff1]==7 && ram[0xdff2]==9 &&
              args_threads.run(nt_iop_generated_run,ram,32,args_services).reason==NativeStop::unmapped_code,
              "Overlapping argument source was not copied before worker execution");
        NativeInterruptServices system_interrupts(0xf000);
        NativeThreadServices system_threads(system_interrupts);
        NativeEventServices system_events(&system_threads);
        const std::array system_modes{NativeBootMode{2,4,{}}};
        NativeBootServices system_boot(system_modes,ram,0xc600,16);
        NativeSystemServices system_service(system_events,system_boot,ram);
        NativeServiceRouter system_services({&system_threads,&system_interrupts,&system_events,&system_service});
        auto system_query=initial(); system_query.pc=0x1974;
        check(run_native(nt_iop_generated_run,system_query,ram,1,&system_services).reason==NativeStop::budget,
              "Compiled GetSystemStatusFlag did not return");
        const auto system_event=system_query.registers[2];
        check(system_events.invoke({13,{system_event,0xc620,0,0},NativeServiceKind::thevent},ram).value==0 &&
              load_native_word(ram,0xc620)==2 && load_native_word(ram,0xc628)==0 && load_native_word(ram,0xc62c)==4,
              "System-status event did not retain multi-waiter attributes and supplied boot-mode bits");
        system_events.create(0,0,123);
        system_query=initial(); system_query.pc=0x1974; system_query.interrupt_depth=1;
        check(run_native(nt_iop_generated_run,system_query,ram,1,&system_services).reason==NativeStop::budget &&
              system_query.registers[2]==system_event,"System-status queries did not retain their shared event handle");
        const auto system_waiter=system_threads.admit(waiting_state(system_event,0,0xc640));
        check(system_threads.run(nt_iop_generated_run,ram,32,system_services).reason==NativeStop::thread_wait,
              "System-status event fabricated readiness for an unset bit");
        check(system_interrupts.invoke({4,{12,2,0x1c00,system_event},NativeServiceKind::intrman},ram).value==0 &&
              system_interrupts.invoke({6,{12,0,0,0},NativeServiceKind::intrman},ram).value==0 &&
              system_interrupts.invoke({9,{},NativeServiceKind::intrman},ram).value==0,"System-status signal setup failed");
        system_interrupts.post(12);
        check(system_threads.run(nt_iop_generated_run,ram,32,system_services).reason==NativeStop::unmapped_code &&
              system_threads.state(system_waiter).registers[2]==0 && load_native_word(ram,0xc640)==12,
              "Compiled signal and wait did not operate on the queried system-status event");
        NativeBootServices empty_system_boot;
        NativeEventServices empty_system_events;
        NativeSystemServices empty_system(empty_system_events,empty_system_boot,ram);
        const auto empty_system_id=empty_system.invoke({41,{},NativeServiceKind::thbase},ram).value;
        check(empty_system_events.invoke({13,{empty_system_id,0xc620,0,0},NativeServiceKind::thevent},ram).value==0 &&
              load_native_word(ram,0xc628)==0 && load_native_word(ram,0xc62c)==0,
              "Empty native boot context fabricated system-status bits");
        {
            NativeVblankServices blank;
            NativeInterruptServices irq(0xf000,&blank);
            NativeServiceRouter services({&irq,&blank});
            auto reg=blank.invoke({8,{0,16,0x2100,0xc720},NativeServiceKind::vblank},ram);
            check(reg.status==NativeServiceStatus::returned && reg.value==0,"Vblank registration failed");
            check(blank.invoke({8,{0,16,0x2100,0xc720},NativeServiceKind::vblank},ram).value==static_cast<std::uint32_t>(-104),"Duplicate vblank registration accepted");
            check(!blank.take(),"Vblank registration invented a display edge");
            store_native_word(ram,0xc720,0);
            blank.post(false);
            auto cpu=initial(); cpu.pc=0xfffffffc;
            check(irq.run(nt_iop_generated_run,cpu,ram,32,services).reason==NativeStop::unmapped_code && load_native_word(ram,0xc720)==0,"Masked display callback executed");
            irq.invoke({9,{},NativeServiceKind::intrman},ram);
            check(irq.run(nt_iop_generated_run,cpu,ram,32,services).reason==NativeStop::unmapped_code && load_native_word(ram,0xc720)==1,"Compiled vblank callback did not execute");
            blank.post(false);
            check(!blank.take(),"Zero-return vblank handler was not removed");
            blank.invoke({8,{1,16,0x2140,0xc720},NativeServiceKind::vblank},ram);
            blank.post(true);
            blank.invoke({9,{1,0x2140},NativeServiceKind::vblank},ram);
            check(!blank.take(),"Released display callback remained queued");
            std::uint64_t ns=0;
            NativeHardTimers timers([&] { return ns; }); timers.connect(irq);
            const auto call=[&](unsigned op,std::uint32_t a=0,std::uint32_t b=0,std::uint32_t c=0) {
                return timers.invoke({op,{a,b,c},NativeServiceKind::timrman},ram);
            };
            const auto handle=call(4,1,16,8).value;
            const auto line=call(16,handle).value;
            check(line==6 && call(4,1,16,8).value==static_cast<std::uint32_t>(-150),"Timer allocation did not own a compatible slot");
            irq.invoke({4,{line,2,0x2100,0xc720},NativeServiceKind::intrman},ram);
            irq.invoke({6,{line},NativeServiceKind::intrman},ram);
            call(11,handle,100);
            check(call(7,handle,0x58).status==NativeServiceStatus::returned,"Periodic native timer setup failed");
            check(call(7,handle,1).status==NativeServiceStatus::unavailable && call(8,handle).value==0x58,"Unsupported gated timer changed state");
            ns=1000; timers.take();
            check(call(10,handle).value==36,"Native timer did not advance from its clock");
            ns=3000; timers.take();
            check(call(10,handle).value==10,"Compare reset lost fractional timer ticks");
            cpu=initial(); cpu.pc=0xfffffffc;
            check(irq.run(nt_iop_generated_run,cpu,ram,32,services).reason==NativeStop::unmapped_code && load_native_word(ram,0xc720)==2,"Timer did not deliver a compiled interrupt handler");
            check(call(6,handle).value==0 && call(16,handle).value==static_cast<std::uint32_t>(-151),"Timer release retained a live handle");
            NativeHardTimers separate([&] { return ns; });
            check(separate.invoke({16,{handle},NativeServiceKind::timrman},ram).value==static_cast<std::uint32_t>(-151),"Timer leaked between contexts");
        }
        {
            std::vector<std::uint8_t> display_ram(0x20000);
            NativeVblankServices blank;
            NativeInterruptServices irq(0xf000,&blank);
            NativeThreadServices workers(irq);
            NativeEventServices events(&workers);
            blank.connect(events,display_ram);
            NativeServiceRouter services({&irq,&workers,&events,&blank});
            const auto waiting=[&](unsigned ordinal) {
                auto cpu=initial(); cpu.pc=0x11014+(ordinal-4)*8; cpu.registers[31]=0xfffffffc;
                return cpu;
            };
            const auto run=[&] {
                return workers.run(nt_iop_generated_run,display_ram,256,services,NativeThreadServices::CompletionPolicy::continue_workers);
            };
            const auto suspended=[&](std::uint32_t id) {
                check(thread_status(workers,id).second==0,"Cannot inspect display waiter");
                return load_native_word(ram,0xc388)==4;
            };
            std::array<std::uint32_t,4> ids;
            for (unsigned index=0;index<4;++index) ids[index]=workers.admit(waiting(index+4));
            check(run().reason==NativeStop::thread_wait,"Vblank waits did not block before the first display edge");
            blank.post(false);
            check(run().reason==NativeStop::thread_wait && suspended(ids[0]),
                  "Masked vblank edge completed a waiter");
            irq.invoke({9,{},NativeServiceKind::intrman},display_ram);
            check(run().reason==NativeStop::thread_wait && !suspended(ids[0]) &&
                  !suspended(ids[2]) && suspended(ids[1]) && suspended(ids[3]),
                  "Vblank start did not complete only start and blank-phase waiters");
            const auto next_start=workers.admit(waiting(4));
            const auto another_start=workers.admit(waiting(4));
            const auto phase=workers.admit(waiting(6));
            check(run().reason==NativeStop::thread_wait && !suspended(phase) &&
                  suspended(next_start),"Historical edge stayed ready or current phase was lost");
            blank.post(true);
            check(run().reason==NativeStop::thread_wait && !suspended(ids[1]) &&
                  !suspended(ids[3]) && suspended(next_start),
                  "Vblank end did not complete end and nonblank-phase waits");
            // Base event publication follows handlers with priority below 128.
            blank.invoke({8,{0,16,0x2140,0xc720},NativeServiceKind::vblank},display_ram);
            blank.post(false);
            const auto callback=blank.take();
            check(callback && callback->address==0x2140 && suspended(next_start),
                  "Vblank event publication overtook an earlier-priority handler");
            blank.finish(0);
            check(run().reason==NativeStop::thread_wait && !suspended(next_start) &&
                  !suspended(another_start),"A display edge failed to complete all registered waiters");
            const auto cancelled=workers.admit(waiting(5));
            check(run().reason==NativeStop::thread_wait,"Next end wait reused a historical edge");
            check(workers.invoke({8,{cancelled},NativeServiceKind::thbase},display_ram).value==0,"Vblank waiter termination failed");
            blank.post(true);
            check(run().reason==NativeStop::thread_wait,"Terminated vblank waiter remained queued");
            auto interrupt_wait=waiting(4); interrupt_wait.interrupt_depth=1;
            check(run_native(nt_iop_generated_run,interrupt_wait,display_ram,1,&services).reason==NativeStop::budget &&
                  interrupt_wait.registers[2]==static_cast<std::uint32_t>(-100),"Vblank wait accepted interrupt context");
            NativeVblankServices unconnected;
            auto missing=waiting(4);
            check(run_native(nt_iop_generated_run,missing,display_ram,4,&unconnected).reason==NativeStop::import_request,
                  "Unconnected display wait fabricated completion");
            for (auto target : {0x111a0U,0x12040U,0x111b0U,0U}) {
                store_native_word(display_ram,0x18000,target);
                auto cpu=initial(); cpu.pc=0x11100; cpu.registers[4]=0x18000; cpu.registers[5]=0;
                const auto stop=run_native(nt_iop_generated_run,cpu,display_ram,64);
                check(stop.reason==NativeStop::unmapped_code &&
                      (target==0x111a0 || target==0x12040 ? cpu.pc==0x5000 && cpu.registers[2]==(target==0x111a0 ? 73U : 87U) :
                       target ? stop.address==target && cpu.pc==0x11118 : cpu.pc==0x5000),
                      "Returned registry callback did not execute or reject an unknown target");
            }
            store_native_word(display_ram,0x18000,0x18100);
            store_native_word(display_ram,0x1810c,0x12040);
            auto nested=initial(); nested.pc=0x11130; nested.registers[4]=0x18000; nested.registers[5]=0;
            check(run_native(nt_iop_generated_run,nested,display_ram,64).reason==NativeStop::unmapped_code &&
                  nested.pc==0x5000 && nested.registers[2]==87,"Nested registry callback did not enter compiled client code");
        }
        std::uint64_t alarm_ticks=0;
        NativeAlarmServices alarms([&]{return alarm_ticks;});
        NativeInterruptServices alarm_interrupts(0xf000,&alarms);
        NativeThreadServices alarm_threads(alarm_interrupts);
        NativeServiceRouter alarm_services({&alarms,&alarm_interrupts,&alarm_threads});
        const auto alarm_call=[&](unsigned ordinal,std::uint32_t a,std::uint32_t b=0,std::uint32_t c=0,bool interrupt=false) {
            auto state=initial(); state.pc=0x197c+(ordinal-34)*8;
            state.registers[4]=a; state.registers[5]=b; state.registers[6]=c; state.interrupt_depth=interrupt ? 1 : 0;
            const auto stop=run_native(nt_iop_generated_run,state,ram,1,&alarm_services);
            return std::pair{stop,state};
        };
        store_native_word(ram,0xc700,0); store_native_word(ram,0xc704,0); store_native_word(ram,0xc720,0);
        auto alarm_register=initial(); alarm_register.pc=0x21c0;
        check(run_native(nt_iop_generated_run,alarm_register,ram,32,&alarm_services).reason==NativeStop::unmapped_code &&
              alarm_register.registers[2]==0 && load_native_word(ram,0xc700)==3686,
              "Compiled alarm registration failed to discover its callback or clamp the interval");
        check(alarm_call(35,0xc700,0x2100,0xc720).second.registers[2]==static_cast<std::uint32_t>(-104),
              "Duplicate alarm callback/userdata pair was accepted");
        foreground=initial(); foreground.pc=0x5000; foreground.hi=12; foreground.lo=34;
        alarm_ticks=3686;
        check(alarm_interrupts.run(nt_iop_generated_run,foreground,ram,32,alarm_services).reason==NativeStop::unmapped_code &&
              !foreground.executed && load_native_word(ram,0xc720)==0,"Masked alarm callback executed");
        alarm_interrupts.invoke({9,{},NativeServiceKind::intrman},ram);
        check(alarm_interrupts.run(nt_iop_generated_run,foreground,ram,0,alarm_services).reason==NativeStop::budget &&
              !foreground.executed,"Zero-budget alarm changed its continuation");
        for (unsigned slice=0;slice<16;++slice) {
            scheduled=alarm_interrupts.run(nt_iop_generated_run,foreground,ram,1,alarm_services);
            if (scheduled.reason!=NativeStop::budget) break;
        }
        check(scheduled.reason==NativeStop::unmapped_code && foreground.executed==7 && foreground.hi==12 && foreground.lo==34 &&
              load_native_word(ram,0xc720)==1 && !alarms.delay(),"One-shot alarm did not preserve context or remove itself");
        check(alarm_call(35,0xc700,0x2140,0xc720).second.registers[2]==0,"Periodic alarm registration failed");
        foreground=initial(); foreground.pc=0x5000; alarm_ticks=7371;
        check(alarm_interrupts.run(nt_iop_generated_run,foreground,ram,32,alarm_services).reason==NativeStop::unmapped_code &&
              !foreground.executed,"Alarm fired before its deadline");
        alarm_ticks=18430;
        check(alarm_interrupts.run(nt_iop_generated_run,foreground,ram,32,alarm_services).reason==NativeStop::unmapped_code &&
              foreground.executed==28 && load_native_word(ram,0xc720)==5,
              "Periodic alarm did not rearm relative to its prior deadline with the minimum interval");
        check(alarm_call(37,0x2140,0xc720).second.registers[2]==0 && !alarms.delay() &&
              alarm_call(37,0x2140,0xc720).second.registers[2]==static_cast<std::uint32_t>(-105),"Alarm cancellation lost its state");
        check(alarm_call(36,0xc700,0x2100,0xc720).second.registers[2]==static_cast<std::uint32_t>(-100) &&
              alarm_call(36,0xc700,0x2100,0xc720,true).second.registers[2]==0 &&
              alarm_call(38,0x2100,0xc720,0,true).second.registers[2]==0,"Interrupt alarm context contract differs");
        alarm_ticks=20000;
        const auto alarm_sleeper=alarm_threads.admit(sleeper);
        check(alarm_threads.run(nt_iop_generated_run,ram,32,alarm_services).reason==NativeStop::thread_wait &&
              alarm_call(35,0xc700,0x2180,alarm_sleeper).second.registers[2]==0,"Alarm wakeup setup failed");
        alarm_ticks=23686;
        check(alarm_threads.run(nt_iop_generated_run,ram,32,alarm_services).reason==NativeStop::unmapped_code &&
              alarm_threads.state(alarm_sleeper).registers[2]==43 && !alarms.delay(),
              "Due alarm did not wake and resume an idle compiled thread");
        check(alarm_call(35,0xc700,0x4000).first.reason==NativeStop::unmapped_code && !alarms.delay(),
              "Unknown alarm handler was registered");
        check(alarm_call(35,0x1000,0x2100).first.reason==NativeStop::code_write && !alarms.delay(),
              "Minimum alarm interval overwrote compiled code");
        alarm_ticks=0x100000002ULL;
        check(alarm_call(34,0xc740).second.registers[2]==0 && load_native_word(ram,0xc740)==2 && load_native_word(ram,0xc744)==1,
              "GetSystemTime lost its 64-bit clock sample");
        alarm_ticks=0xfffffffffffffff0ULL;
        check(alarm_call(35,0xc700,0x2100).first.reason==NativeStop::import_request && !alarms.delay(),
              "Alarm deadline overflow wrapped into immediate execution");
        const auto clock_reply=[&](unsigned ordinal,std::uint32_t a,std::uint32_t b=0,std::uint32_t c=0,bool interrupt=false) {
            return alarms.invoke({ordinal,{a,b,c,0},NativeServiceKind::thbase,0,interrupt},ram);
        };
        auto usec_clock=clock_reply(39,1000000,0xc760);
        check(usec_clock.status==NativeServiceStatus::returned && usec_clock.value==0 &&
              load_native_word(ram,0xc760)==36864000 && load_native_word(ram,0xc764)==0,
              "USec2SysClock did not convert one million microseconds at the default frequency");
        auto back_clock=clock_reply(40,0xc760,0xc768,0xc76c,true);
        check(back_clock.status==NativeServiceStatus::returned && back_clock.value==0 &&
              load_native_word(ram,0xc768)==1 && load_native_word(ram,0xc76c)==0,
              "SysClock2USec did not round-trip one second in interrupt context");
        store_native_word(ram,0xc760,36864); store_native_word(ram,0xc764,0);
        back_clock=clock_reply(40,0xc760,0xc768,0xc76c);
        check(back_clock.status==NativeServiceStatus::returned && load_native_word(ram,0xc768)==0 &&
              load_native_word(ram,0xc76c)==1000,
              "SysClock2USec did not convert 36864 ticks to 1000 microseconds");
        store_native_word(ram,0xc780,36864); store_native_word(ram,0xc784,0);
        back_clock=clock_reply(40,0xc780,0xc780,0xc784);
        check(back_clock.status==NativeServiceStatus::returned && load_native_word(ram,0xc780)==0 &&
              load_native_word(ram,0xc784)==1000,
              "SysClock2USec aliased its tick input with the second and microsecond outputs");
        store_native_word(ram,0xc790,0); store_native_word(ram,0xc794,36864000);
        store_native_word(ram,0xc798,0x11111111); store_native_word(ram,0xc79c,0x22222222);
        const auto overflow_clock=clock_reply(40,0xc790,0xc798,0xc79c);
        check(overflow_clock.status==NativeServiceStatus::unsupported &&
              load_native_word(ram,0xc798)==0x11111111 && load_native_word(ram,0xc79c)==0x22222222,
              "SysClock2USec wrapped a second count that does not fit in 32 bits");
        const auto fault_clock=clock_reply(39,1,0xc761);
        check(fault_clock.status==NativeServiceStatus::address_fault && fault_clock.address==0xc761,
              "USec2SysClock accepted an unaligned clock pointer");
        const auto protected_clock=clock_reply(39,1,0x1000);
        check(protected_clock.status==NativeServiceStatus::code_write && protected_clock.address==0x1000,
              "USec2SysClock wrote a clock over compiled code");
        std::fill(ram.begin()+0xcb00,ram.begin()+0xcb10,static_cast<std::uint8_t>(0xab));
        const auto sample=std::chrono::sys_days{std::chrono::year{2024}/std::chrono::March/5}+
            std::chrono::hours{3}+std::chrono::minutes{4}+std::chrono::seconds{5};
        NativeCdvdmanServices cdvd([sample] { return sample; });
        const auto read_clock=cdvd.invoke({24,{0xcb00},NativeServiceKind::cdvdman},ram);
        check(read_clock.status==NativeServiceStatus::returned && read_clock.value==1 &&
              ram[0xcb00]==0 && ram[0xcb01]==0x05 && ram[0xcb02]==0x04 && ram[0xcb03]==0x12 &&
              ram[0xcb04]==0 && ram[0xcb05]==0x05 && ram[0xcb06]==0x03 && ram[0xcb07]==0x24 &&
              ram[0xcb08]==0xab,
              "sceCdReadClock did not write the JST BCD clock");
        const auto stale_year=std::chrono::sys_days{std::chrono::year{1999}/std::chrono::January/1};
        NativeCdvdmanServices stale([stale_year] { return stale_year; });
        const auto rejected_clock=stale.invoke({24,{0xcb00},NativeServiceKind::cdvdman},ram);
        check(rejected_clock.status==NativeServiceStatus::unsupported && ram[0xcb00]==0 && ram[0xcb07]==0x24,
              "sceCdReadClock replaced a clock for a year outside 2000-2099");
        const auto missed_clock=cdvd.invoke({24,{0xcb01},NativeServiceKind::cdvdman},ram);
        check(missed_clock.status==NativeServiceStatus::address_fault && missed_clock.address==0xcb01 && ram[0xcb01]==0x05,
              "sceCdReadClock wrote through an unaligned pointer");
        check(cdvd.invoke({25,{0xcb00},NativeServiceKind::cdvdman},ram).status==NativeServiceStatus::unsupported,
              "An unbound cdvdman ordinal was accepted");
        {
            std::vector<std::uint8_t> data(0x20000);
            NativeCdvdmanServices lifecycle;
            auto registration=initial(); registration.pc=0x17990; registration.registers[28]=0x18340;
            check(run_native(nt_iop_generated_run,registration,data,64,&lifecycle).reason==NativeStop::import_request &&
                  !lifecycle.request_close(),"An unattached application accepted power lifecycle calls");
            lifecycle.enable_application_exit();
            registration=initial(); registration.pc=0x17990; registration.registers[28]=0x18340;
            check(run_native(nt_iop_generated_run,registration,data,64,&lifecycle).reason==NativeStop::unmapped_code &&
                  registration.pc==0x5000 && registration.registers[2]==0 && !lifecycle.exit_requested(),
                  "Power callback registration was not compiled or exited prematurely");
            const auto original=data;
            for (const auto address:{0U,0x1ffffU,0x1000U}) {
                auto power=initial(); power.pc=0x1786c; power.registers[4]=address;
                const auto result=run_native(nt_iop_generated_run,power,data,8,&lifecycle);
                check(result.reason==(address==0x1000 ? NativeStop::code_write : NativeStop::address_fault) &&
                      !power.executed && !lifecycle.exit_requested() && data==original,
                      "Invalid native exit output changed data or requested exit");
            }
            auto invalid=initial(); invalid.pc=0x17864; invalid.registers[4]=0x17b80;
            check(run_native(nt_iop_generated_run,invalid,data,8,&lifecycle).reason==NativeStop::unmapped_code &&
                  !invalid.executed,"Power callback accepted uncompiled code");
            check(lifecycle.request_close() && lifecycle.request_close() && !lifecycle.exit_requested(),
                  "Window close bypassed the registered callback");
            NativeCallbackMux callbacks({&lifecycle});
            NativeInterruptServices interrupts(0x1f000,&callbacks);
            NativeServiceRouter router({&interrupts,&lifecycle});
            interrupts.invoke({9,{},NativeServiceKind::intrman},data);
            auto foreground=initial(); foreground.pc=0x5000;
            const auto result=interrupts.run(nt_iop_generated_run,foreground,data,64,router);
            check(result.reason==NativeStop::unmapped_code && foreground.pc==0x5000 && lifecycle.exit_requested() &&
                  load_native_word(data,0x1820c)==42 && load_native_word(data,0x18210)==0x18340 &&
                  load_native_word(data,0x18214)==0 && !lifecycle.take(),
                  "Native close callback lost argument, GP, completion or one-shot ownership");
            NativeCdvdmanServices removed; removed.enable_application_exit();
            registration=initial(); registration.pc=0x17990;
            run_native(nt_iop_generated_run,registration,data,64,&removed);
            auto remove=initial(); remove.pc=0x17864; remove.registers[4]=0;
            check(run_native(nt_iop_generated_run,remove,data,8,&removed).reason==NativeStop::unmapped_code &&
                  remove.registers[2]==0x17960 && removed.request_close() && removed.exit_requested() && !removed.take(),
                  "Removed close callback retained execution or lost its previous pointer");
        }
        NativeSecrmanServices secrman;
        const auto command=secrman.invoke({4,{0x80001234},NativeServiceKind::secrman},ram);
        const auto device=secrman.invoke({5,{0x80005678},NativeServiceKind::secrman},ram);
        const auto auth=secrman.invoke({6,{2,0,1},NativeServiceKind::secrman},ram);
        check(command.status==NativeServiceStatus::returned && command.value==0 &&
              device.status==NativeServiceStatus::returned && device.value==0 &&
              auth.status==NativeServiceStatus::returned && auth.value==0 &&
              secrman.command_handler()==0x80001234 && secrman.device_handler()==0x80005678,
              "secrman handler registration or AuthCard reported success");
        const auto cleared=secrman.invoke({4,{0},NativeServiceKind::secrman},ram);
        const auto cleared_device=secrman.invoke({5,{0},NativeServiceKind::secrman},ram);
        check(cleared.value==0 && cleared_device.value==0 && secrman.command_handler()==0 && secrman.device_handler()==0 &&
              secrman.invoke({8,{},NativeServiceKind::secrman},ram).status==NativeServiceStatus::unsupported,
              "secrman did not clear a handler or accepted a download ordinal");
        std::vector<std::uint8_t> ee_memory(0x1000,0);
        NativeSifDestination ee_destination{ee_memory,[](auto address,auto size) { return address>=0x100 && size<=0x1000-address; }};
        NativeSifTransfers sif(&ee_destination),disconnected_sif;
        std::uint64_t transport_ticks=0;
        NativeAlarmServices transport_alarms([&]{return transport_ticks;});
        NativeCallbackMux transport_callbacks({&transport_alarms,&sif});
        NativeInterruptServices transport_interrupts(0xf000,&transport_callbacks);
        NativeServiceRouter transport_services({&sif,&transport_alarms,&transport_interrupts});
        const auto sif_call=[&](unsigned ordinal,std::uint32_t a=0,std::uint32_t b=0,std::uint32_t c=0,std::uint32_t d=0) {
            const unsigned ordinals[]{7,8,30,31,32};
            const auto at=std::find(std::begin(ordinals),std::end(ordinals),ordinal);
            auto state=initial(); state.pc=0x2214+static_cast<std::uint32_t>(at-std::begin(ordinals))*8;
            state.registers[4]=a; state.registers[5]=b; state.registers[6]=c; state.registers[7]=d;
            const auto stop=run_native(nt_iop_generated_run,state,ram,1,&transport_services);
            return std::pair{stop,state};
        };
        for (unsigned index=0;index<32;++index) ram[0xc800+index]=static_cast<std::uint8_t>(index+1);
        const auto transfer_descriptor=[&](unsigned index,std::uint32_t source,std::uint32_t destination,std::uint32_t size,std::uint32_t attributes=0) {
            const auto at=0xc840+index*16;
            store_native_word(ram,at,source); store_native_word(ram,at+4,destination);
            store_native_word(ram,at+8,size); store_native_word(ram,at+12,attributes);
        };
        transfer_descriptor(0,0x8000c800,0x80000100,16);
        transfer_descriptor(1,0xc810,0x120,16,2);
        auto no_peer=initial(); no_peer.pc=0x2214; no_peer.registers[4]=0xc840; no_peer.registers[5]=2;
        check(run_native(nt_iop_generated_run,no_peer,ram,1,&disconnected_sif).reason==NativeStop::import_request && !no_peer.executed,
              "Disconnected native SIF endpoint fabricated transfer success");
        store_native_word(ram,0xc880,0); store_native_word(ram,0xc884,0);
        check(sif_call(30,0x2100,0xc880).second.registers[2]==0,"Native SIF general completion registration failed");
        const auto transferred=sif_call(32,0xc840,2,0x2140,0xc884);
        check(transferred.first.reason==NativeStop::budget && transferred.second.registers[2]>0 &&
              std::equal(ram.begin()+0xc800,ram.begin()+0xc810,ee_memory.begin()+0x100) &&
              std::equal(ram.begin()+0xc810,ram.begin()+0xc820,ee_memory.begin()+0x120) &&
              sif_call(8,transferred.second.registers[2]).second.registers[2]==0xffffffff,
              "Compiled SIF batch did not copy both payloads or report completed status");
        foreground=initial(); foreground.pc=0x5000;
        check(transport_interrupts.run(nt_iop_generated_run,foreground,ram,32,transport_services).reason==NativeStop::unmapped_code &&
              !foreground.executed,"SIF completion ignored the global interrupt mask");
        transport_interrupts.invoke({9,{},NativeServiceKind::intrman},ram);
        for (unsigned slice=0;slice<32;++slice) {
            scheduled=transport_interrupts.run(nt_iop_generated_run,foreground,ram,1,transport_services);
            if (scheduled.reason!=NativeStop::budget) break;
        }
        check(scheduled.reason==NativeStop::unmapped_code && foreground.executed==14 &&
              load_native_word(ram,0xc880)==1 && load_native_word(ram,0xc884)==1,
              "SIF completion callbacks lost slice state or incorrectly treated a void result as an alarm repeat");
        const auto prior_ee=ee_memory;
        transfer_descriptor(1,0xc810,0xff0,32);
        check(sif_call(7,0xc840,2).first.reason==NativeStop::address_fault && ee_memory==prior_ee && !sif.take(),
              "Invalid later SIF descriptor committed an earlier payload or completion");
        transfer_descriptor(1,0xc810,0x40,16);
        check(sif_call(7,0xc840,2).first.reason==NativeStop::code_write && ee_memory==prior_ee,
              "Native SIF transfer overwrote protected EE code");
        transfer_descriptor(1,0xc810,0x120,16,4);
        check(sif_call(7,0xc840,2).first.reason==NativeStop::import_request && ee_memory==prior_ee,
              "Native SIF transfer silently accepted an unsupported EE interrupt attribute");
        check(sif.rejected_transfer() && sif.rejected_transfer()->index==1 && sif.rejected_transfer()->attributes==4,
              "Rejected SIF descriptor diagnostics lost");
        transfer_descriptor(1,0xc804,0x120,16);
        check(sif_call(7,0xc840,2).first.reason==NativeStop::budget && !sif.rejected_transfer() &&
              std::equal(ram.begin()+0xc804,ram.begin()+0xc814,ee_memory.begin()+0x120),
              "Word-aligned IOP SIF source incorrectly required quadword alignment");
        check(sif.take().has_value(),"Word-aligned SIF transfer lost completion"); sif.finish(0);
        const auto aligned_ee=ee_memory;
        transfer_descriptor(1,0xc805,0x120,16);
        check(sif_call(7,0xc840,2).first.reason==NativeStop::import_request && ee_memory==aligned_ee && !sif.take(),
              "Misaligned source partially committed a SIF batch");
        ee_memory=prior_ee;
        transfer_descriptor(1,0xc810,0x120,16);
        check(sif_call(32,0xc840,2,0x4000).first.reason==NativeStop::unmapped_code && ee_memory==prior_ee,
              "Unknown SIF completion handler allowed a partial transfer");
        check(sif_call(7,0xc840,1).second.registers[2]>0 && sif_call(31).second.registers[2]==0,
              "Native SIF completion reset setup failed");
        foreground=initial(); foreground.pc=0x5000;
        check(transport_interrupts.run(nt_iop_generated_run,foreground,ram,32,transport_services).reason==NativeStop::unmapped_code &&
              !foreground.executed && load_native_word(ram,0xc880)==1,"Reset general handler still executed from a queued completion");
        store_native_word(ram,0xc888,0); store_native_word(ram,0xc88c,0);
        auto mixed_alarm=initial(); mixed_alarm.pc=0x1984;
        mixed_alarm.registers[4]=0xc700; mixed_alarm.registers[5]=0x2100; mixed_alarm.registers[6]=0xc888;
        check(run_native(nt_iop_generated_run,mixed_alarm,ram,1,&transport_services).reason==NativeStop::budget &&
              mixed_alarm.registers[2]==0 && sif_call(32,0xc840,2,0x2140,0xc88c).second.registers[2]>0,
              "Mixed native completion sources could not enqueue work");
        transport_ticks=3686; foreground=initial(); foreground.pc=0x5000;
        check(transport_interrupts.run(nt_iop_generated_run,foreground,ram,32,transport_services).reason==NativeStop::unmapped_code &&
              foreground.executed==14 && load_native_word(ram,0xc888)==1 && load_native_word(ram,0xc88c)==1 && !transport_alarms.delay(),
              "Callback multiplexing lost source ownership between an alarm and a SIF completion");
        NativeInterruptServices receive_interrupts(0xf000);
        NativeSifControl link(receive_interrupts,&sif);
        NativeServiceRouter link_services({&link,&receive_interrupts});
        const auto link_call=[&](unsigned ordinal,std::uint32_t value=0) {
            const unsigned ordinals[]{7,8,30,31,32,5,6,21,22,23,24,25,26,27,29,2};
            const auto at=std::find(std::begin(ordinals),std::end(ordinals),ordinal);
            auto state=initial(); state.pc=0x2214+static_cast<std::uint32_t>(at-std::begin(ordinals))*8; state.registers[4]=value;
            const auto stop=run_native(nt_iop_generated_run,state,ram,1,&link_services);
            return std::pair{stop,state};
        };
        check(link_call(29).second.registers[2]==0 && link_call(5).first.reason==NativeStop::import_request,
              "Disconnected SIF control fabricated an initialized link");
        check(link_call(23).second.registers[2]==0 && link_call(24,0x40000).second.registers[2]==0x40000 &&
              link_call(24,0x80000).second.registers[2]==0xc0000 && link_call(23).second.registers[2]==0xc0000 &&
              link_call(5).first.reason==NativeStop::import_request && link_call(21).first.reason==NativeStop::import_request &&
              link_call(29).second.registers[2]==0 && !link.ready(),
              "Local readiness publication fabricated a peer or lost flags before connection");
        link.publish_main(0x120,0x20000);
        check(link_call(23).second.registers[2]==0xc0000,"Peer connection erased previously published local readiness");
        link.acknowledge_sub(0xc0000);
        check(link_call(5).first.reason==NativeStop::import_request && link_call(29).second.registers[2]==0,
              "SIF initialization ignored the peer readiness handshake");
        link.publish_main(0x120,0x10000);
        check(link_call(5).second.registers[2]==0 && link_call(29).second.registers[2]==1 &&
              link_call(21).second.registers[2]==0x30000 && link_call(23).second.registers[2]==0x10000,
              "Native SIF initialization did not publish its actual handshake state");
        check(link_call(22,0x20000).second.registers[2]==0x10000 && link_call(24,0x20000).second.registers[2]==0x30000,
              "Native SIF publish/acknowledge directions differ");
        link.acknowledge_sub(0x10000);
        check(link_call(23).second.registers[2]==0x20000 && link_call(25).second.registers[2]==0x120 &&
              link_call(27,0xa000c900).second.registers[2]==0xa000c900 && link_call(5).second.registers[2]==0 &&
              link_call(26).second.registers[2]==0xa000c900,"Repeated SIF init reset command-buffer coordination");
        std::array<std::uint8_t,16> incoming{}; incoming[0]=10;
        check(link.receive(incoming,0x1000,ram).status==NativeServiceStatus::code_write &&
              link.receive(incoming,0xfff0+16,ram).status==NativeServiceStatus::address_fault,
              "Inbound SIF accepted invalid/protected destinations");
        check(receive_interrupts.invoke({4,{43,1,0x2140,0xc900},NativeServiceKind::intrman},ram).value==0 &&
              receive_interrupts.invoke({6,{43,0,0,0},NativeServiceKind::intrman},ram).value==0 &&
              link.receive(incoming,link.sub_address(),ram).status==NativeServiceStatus::returned && load_native_word(ram,0xc900)==10,
              "Native inbound SIF did not commit data before notification");
        foreground=initial(); foreground.pc=0x5000;
        check(receive_interrupts.run(nt_iop_generated_run,foreground,ram,32,link_services).reason==NativeStop::unmapped_code &&
              !foreground.executed && load_native_word(ram,0xc900)==10,"Inbound SIF notification bypassed the global mask");
        receive_interrupts.invoke({9,{},NativeServiceKind::intrman},ram);
        check(receive_interrupts.run(nt_iop_generated_run,foreground,ram,32,link_services).reason==NativeStop::unmapped_code &&
              foreground.executed==7 && load_native_word(ram,0xc900)==11,"Compiled SIF1 handler did not observe received bytes");
        incoming[0]=20;
        check(link.receive(incoming,link.sub_address(),ram).status==NativeServiceStatus::unsupported && load_native_word(ram,0xc900)==11,
              "Inbound SIF overwrote data without receive rearming");
        check(link_call(6).second.registers[2]==0 && link.receive(incoming,link.sub_address(),ram).status==NativeServiceStatus::returned,
              "Compiled SetDChain did not rearm native receive delivery");
        foreground=initial(); foreground.pc=0x5000;
        check(receive_interrupts.run(nt_iop_generated_run,foreground,ram,32,link_services).reason==NativeStop::unmapped_code &&
              foreground.executed==7 && load_native_word(ram,0xc900)==21,"Rearmed inbound SIF did not notify its compiled consumer");
        // Teardown is exercised through compiled imports with outstanding work.
        check(sif_call(32,0xc840,1,0x2140,0xc88c).second.registers[2]>0 && sif.take().has_value(),
              "SIF teardown active-completion setup failed");
        check(link_call(2).first.reason==NativeStop::import_request && link_call(6).second.registers[2]==0,
              "SIF teardown discarded an active completion or partially stopped reception");
        sif.finish(0);
        check(sif_call(32,0xc840,1,0x2140,0xc88c).second.registers[2]>0 &&
              receive_interrupts.invoke({4,{42,1,0x2140,0xc900},NativeServiceKind::intrman},ram).value==0 &&
              receive_interrupts.invoke({6,{42,0,0,0},NativeServiceKind::intrman},ram).value==0,
              "SIF teardown pending-work setup failed");
        receive_interrupts.post(42);
        auto interrupt_cleanup=initial(); interrupt_cleanup.pc=0x228c; interrupt_cleanup.interrupt_depth=1;
        check(run_native(nt_iop_generated_run,interrupt_cleanup,ram,1,&link_services).reason==NativeStop::import_request &&
              !interrupt_cleanup.executed,"SIF teardown accepted interrupt context");
        const auto committed_ee=ee_memory;
        check(link_call(2).second.registers[2]==0 && link_call(2).second.registers[2]==0 &&
              link_call(29).second.registers[2]==1 && !sif.take() &&
              sif_call(7,0xc840,1).first.reason==NativeStop::import_request && ee_memory==committed_ee &&
              link.receive(incoming,0xc900,ram).status==NativeServiceStatus::unsupported &&
              link_call(6).first.reason==NativeStop::import_request,
              "SIF teardown lost its init marker, retained work or fabricated reconnection");
        foreground=initial(); foreground.pc=0x5000;
        check(receive_interrupts.run(nt_iop_generated_run,foreground,ram,32,link_services).reason==NativeStop::unmapped_code &&
              !foreground.executed && receive_interrupts.invoke({5,{42,0,0,0},NativeServiceKind::intrman},ram).value==
              static_cast<std::uint32_t>(-105),"SIF teardown retained the SIF0 handler or delivered cancelled work");
        NativeSifControl empty_link(receive_interrupts,&disconnected_sif);
        auto empty_cleanup=initial(); empty_cleanup.pc=0x228c;
        check(run_native(nt_iop_generated_run,empty_cleanup,ram,1,&empty_link).reason==NativeStop::budget &&
              empty_cleanup.registers[2]==0,"Disconnected SIF cleanup required an invented peer");
        {
            NativeSifCommands defaults;
            NativeInterruptServices default_interrupts(0xf000,&defaults);
            NativeServiceRouter default_services({&defaults,&default_interrupts});
            const auto default_call=[&](unsigned ordinal,std::uint32_t id,std::uint32_t handler=0) {
                auto state=initial(); state.pc=0x2414+(ordinal-8)*8;
                state.registers[4]=id; state.registers[5]=handler; state.registers[6]=0xcf00;
                return run_native(nt_iop_generated_run,state,ram,1,&default_services);
            };
            check(default_call(10,0x8000001f,0x24c0).reason==NativeStop::import_request,
                  "Default system handlers were usable before native command initialization");
            const auto before_default_setup=ram;
            defaults.initialize_system_handlers();
            check(default_call(10,0x8000001f,0x24c0).reason==NativeStop::budget && ram==before_default_setup &&
                  defaults.uses_builtin_handler(0x80000001) && !defaults.uses_builtin_handler(0x8000001f),
                  "Native default registration changed guest storage or disconnected unrelated protocol handlers");
            defaults.initialize_system_handlers(); // Repeated initialization must preserve live registrations.
            check(default_call(10,0x80000020,0x24c0).reason==NativeStop::import_request &&
                  default_call(10,0x1f,0x24c0).reason==NativeStop::import_request &&
                  default_call(10,0x8000001f,0x4000).reason==NativeStop::unmapped_code,
                  "Default system registration accepted an invalid index, user slot or uncompiled callback");
            std::array<std::uint8_t,32> default_packet{};
            store_native_word(default_packet,0,32); store_native_word(default_packet,8,0x8000001f);
            store_native_word(default_packet,16,7); store_native_word(ram,0xcf00,10);
            check(defaults.receive(default_packet,0xce00,ram).value==1 &&
                  default_call(11,0x8000001f).reason==NativeStop::budget,
                  "Native default command did not retain its accepted callback across removal");
            default_interrupts.invoke({9,{},NativeServiceKind::intrman},ram);
            auto default_foreground=initial(); default_foreground.pc=0x5000;
            check(default_interrupts.run(nt_iop_generated_run,default_foreground,ram,32,default_services).reason==NativeStop::unmapped_code &&
                  load_native_word(ram,0xcf00)==17 && !defaults.busy() &&
                  defaults.receive(default_packet,0xce00,ram).value==0 && !defaults.busy(),
                  "Default command callback was lost, repeated or retained after removal");
            check(default_call(11,0x80000001).reason==NativeStop::budget &&
                  !defaults.uses_builtin_handler(0x80000001) && defaults.uses_builtin_handler(0x80000002),
                  "Removing one default protocol handler affected unrelated handlers");
            NativeSifCommands new_lifetime;
            new_lifetime.initialize_system_handlers();
            check(new_lifetime.uses_builtin_handler(0x80000001) &&
                  new_lifetime.receive(default_packet,0xce00,ram).value==0 && !new_lifetime.busy(),
                  "A new native command lifetime inherited old removals or callbacks");
        }
        NativeSifCommands commands;
        NativeInterruptServices command_interrupts(0xf000,&commands);
        NativeServiceRouter command_services({&commands,&command_interrupts});
        const auto command_call=[&](unsigned ordinal,std::uint32_t a=0,std::uint32_t b=0,std::uint32_t c=0) {
            auto state=initial(); state.pc=0x2414+(ordinal-8)*8;
            state.registers[4]=a; state.registers[5]=b; state.registers[6]=c;
            const auto stop=run_native(nt_iop_generated_run,state,ram,1,&command_services);
            return std::pair{stop,state};
        };
        check(command_call(8,0x8000cc00,4).second.registers[2]==0 &&
              command_call(9,0xcc80,4).second.registers[2]==0,"Native command table setup failed");
        auto register_command=initial(); register_command.pc=0x2480;
        check(run_native(nt_iop_generated_run,register_command,ram,32,&command_services).reason==NativeStop::unmapped_code &&
              register_command.pc==0x5000 && load_native_word(ram,0xcc08)==0x24c0 && load_native_word(ram,0xcc0c)==0xcf00,
              "Compiled command registration did not discover/store its private callback");
        std::array<std::uint8_t,32> packet{}; packet[0]=32; packet[8]=1; packet[16]=7;
        store_native_word(ram,0xcf00,10);
        check(commands.receive(packet,0x8000ce00,ram).value==1 && commands.busy(),"Native command receive did not queue a callback");
        const auto held_packet=std::vector<std::uint8_t>(ram.begin()+0xce00,ram.begin()+0xce20);
        auto replacement=packet; replacement[16]=99;
        check(commands.receive(replacement,0xce00,ram).status==NativeServiceStatus::unsupported &&
              std::equal(held_packet.begin(),held_packet.end(),ram.begin()+0xce00),"Busy command receive overwrote an accepted packet");
        foreground=initial(); foreground.pc=0x5000; foreground.registers[5]=0x12345678;
        check(command_interrupts.run(nt_iop_generated_run,foreground,ram,32,command_services).reason==NativeStop::unmapped_code &&
              !foreground.executed && commands.busy(),"Command callback bypassed the interrupt mask");
        command_interrupts.invoke({9,{},NativeServiceKind::intrman},ram);
        for (unsigned slice=0;slice<16;++slice) {
            scheduled=command_interrupts.run(nt_iop_generated_run,foreground,ram,1,command_services);
            if (scheduled.reason!=NativeStop::budget) break;
        }
        check(scheduled.reason==NativeStop::unmapped_code && foreground.executed==7 && load_native_word(ram,0xcf00)==17 &&
              foreground.registers[5]==0x12345678 && !commands.busy(),"Native command callback lost its second argument, foreground state or completion");
        store_native_word(ram,0xcc94,0x12345678); // System table's third word belongs to its caller.
        check(command_call(10,0x80000001,0x800024c0,0xcf00).second.registers[2]==0 &&
              load_native_word(ram,0xcc8c)==0x800024c0 && load_native_word(ram,0xcc94)==0x12345678,
              "System command table stride or reserved field differs");
        packet[11]=0x80;
        check(commands.receive(packet,0xce00,ram).value==1,"System command receive failed");
        foreground=initial(); foreground.pc=0x5000;
        check(command_interrupts.run(nt_iop_generated_run,foreground,ram,32,command_services).reason==NativeStop::unmapped_code &&
              load_native_word(ram,0xcf00)==24,"Compiled system command callback did not run");
        check(command_call(11,0x80000001).second.registers[2]==0 && load_native_word(ram,0xcc8c)==0 &&
              load_native_word(ram,0xcc90)==0 && load_native_word(ram,0xcc94)==0x12345678 &&
              commands.receive(packet,0xce00,ram).value==0 && !commands.busy(),"Removed command still queued a callback");
        packet[11]=0;
        check(command_call(10,1,0x4000,0).first.reason==NativeStop::unmapped_code && load_native_word(ram,0xcc08)==0x24c0,
              "Unknown command handler changed registration");
        check(command_call(8,0xfff0,4).first.reason==NativeStop::address_fault &&
              command_call(10,4,0x24c0,0).first.reason==NativeStop::import_request,
              "Invalid command table extent or index was accepted");
        check(commands.receive(packet,0x2400,ram).status==NativeServiceStatus::code_write &&
              commands.receive(packet,0xfff0,ram).status==NativeServiceStatus::address_fault &&
              commands.receive(packet,0xcc00,ram).status==NativeServiceStatus::unsupported && !commands.busy(),
              "Invalid command packet destination changed state");
        packet[1]=1;
        check(commands.receive(packet,0xce00,ram).status==NativeServiceStatus::unsupported && !commands.busy(),
              "Command receive fabricated an extra-data transfer");
        packet[1]=0; packet[0]=16;
        check(commands.receive(packet,0xce00,ram).status==NativeServiceStatus::unsupported,"Malformed command size was accepted");
        packet[0]=32;
        store_native_word(ram,0xcc08,0x4000);
        check(commands.receive(packet,0xce00,ram).status==NativeServiceStatus::unmapped_code && !commands.busy(),
              "Mutable command table bypassed native target validation");
        check(command_call(8,0x240c,4).second.registers[2]==0 &&
              command_call(10,1,0x24c0,0).first.reason==NativeStop::code_write,
              "Command registration overwrote generated instructions");
        std::vector<std::uint8_t> command_peer_memory(0x1000,0);
        NativeSifDestination command_destination{command_peer_memory,[](std::uint32_t address,std::uint32_t) { return address>=0x80; }};
        NativeSifTransfers command_completions(&command_destination);
        NativeSifCommandEndpoint command_peer(command_destination,0x80000100,command_completions);
        NativeSifCommands sender(&command_peer);
        const auto send_command=[&](unsigned ordinal=12,bool irq=false,unsigned packet_size=32,NativeServices* service=nullptr) {
            auto state=initial(); state.pc=ordinal==12 ? 0x2434 : ordinal==13 ? 0x243c : ordinal==28 ? 0x2444 : 0x244c; state.interrupt_depth=irq ? 1 : 0;
            state.registers[4]=0x8000000a; state.registers[5]=0x8000d100; state.registers[6]=packet_size;
            state.registers[7]=0xa000d200; state.registers[29]=0xd300;
            const auto stop=run_native(nt_iop_generated_run,state,ram,1,service ? service : &sender);
            return std::pair{stop,state};
        };
        for (unsigned index=0;index<32;++index) ram[0xd100+index]=static_cast<std::uint8_t>(index+1);
        for (unsigned index=0;index<19;++index) ram[0xd200+index]=static_cast<std::uint8_t>(index+91);
        store_native_word(ram,0xd310,0xa0000300); store_native_word(ram,0xd314,19);
        const auto unsent_header=load_native_word(ram,0xd100);
        check(send_command(12,false,32,&commands).first.reason==NativeStop::import_request &&
              load_native_word(ram,0xd100)==unsent_header,"Disconnected command send fabricated success or changed the header");
        check(send_command(12,true).first.reason==NativeStop::import_request &&
              load_native_word(ram,0xd100)==unsent_header,"Foreground command send ran in interrupt context");
        const auto sent=send_command();
        check(sent.first.reason==NativeStop::budget && sent.second.registers[2]>0 &&
              load_native_word(ram,0xd100)==(19U<<8|32) && load_native_word(ram,0xd104)==0xa0000300 &&
              load_native_word(ram,0xd108)==0x8000000a && load_native_word(ram,0xd10c)==0x100f0e0d &&
              std::equal(ram.begin()+0xd200,ram.begin()+0xd213,command_peer_memory.begin()+0x300),
              "Native command send did not marshal stack arguments, preserve opt or copy extra data");
        const auto delivered_memory=command_peer_memory;
        check(send_command().second.registers[2]==0 && command_peer_memory==delivered_memory,
              "Busy native peer overwrote its pending command");
        auto notification=command_peer.take();
        check(notification && notification->size()==32 && load_native_word(*notification,8)==0x8000000a &&
              std::equal(notification->begin(),notification->end(),command_peer_memory.begin()+0x100) && !command_peer.take(),
              "Command notification did not own the committed packet");
        store_native_word(ram,0xd310,0x40);
        const auto valid_header=std::vector<std::uint8_t>(ram.begin()+0xd100,ram.begin()+0xd120);
        check(send_command().first.reason==NativeStop::code_write && command_peer_memory==delivered_memory &&
              std::equal(valid_header.begin(),valid_header.end(),ram.begin()+0xd100) && !command_peer.take(),
              "Protected extra-data destination allowed a partial command send");
        store_native_word(ram,0xd310,0xff0);
        check(send_command().first.reason==NativeStop::address_fault && command_peer_memory==delivered_memory && !command_peer.take(),
              "Invalid peer destination committed command bytes");
        store_native_word(ram,0xd310,0x300); store_native_word(ram,0xd314,0xffffffff);
        check(send_command(13,true,17).second.registers[2]>sent.second.registers[2] &&
              load_native_word(ram,0xd100)==17 && load_native_word(ram,0xd104)==0,
              "Interrupt command send or nonpositive extra-data size differs");
        notification=command_peer.take();
        check(notification && notification->size()==17,"Native command transport imposed artificial DMA padding");
        check(send_command(12,false,15).second.registers[2]==0 && !command_peer.take(),"Undersized command packet was delivered");
        auto bad_stack=initial(); bad_stack.pc=0x2434; bad_stack.registers[5]=0xd100; bad_stack.registers[6]=32;
        bad_stack.registers[29]=0xfffffff0;
        check(run_native(nt_iop_generated_run,bad_stack,ram,1,&sender).reason==NativeStop::address_fault &&
              !bad_stack.executed && !command_peer.take(),"Wrapped command argument stack was accepted");
        store_native_word(ram,0xd314,16);
        auto overlap_send=initial(); overlap_send.pc=0x2434; overlap_send.registers[4]=7; overlap_send.registers[5]=0xd100;
        overlap_send.registers[6]=32; overlap_send.registers[7]=0xd100; overlap_send.registers[29]=0xd300;
        check(run_native(nt_iop_generated_run,overlap_send,ram,1,&sender).reason==NativeStop::budget &&
              std::equal(ram.begin()+0xd100,ram.begin()+0xd110,command_peer_memory.begin()+0x300),
              "Overlapping command/payload buffers did not observe the updated header");
        command_peer.take();
        NativeSifCommandEndpoint protected_peer(command_destination,0x40,command_completions);
        NativeSifCommands protected_sender(&protected_peer);
        const auto prior_command_memory=command_peer_memory;
        check(send_command(12,false,32,&protected_sender).first.reason==NativeStop::code_write &&
              command_peer_memory==prior_command_memory && !protected_peer.take(),"Protected command buffer committed extra data first");
        check(!command_completions.take(),"Unregistered command completion unexpectedly produced a callback");
        NativeInterruptServices send_interrupts(0xf000,&command_completions);
        NativeSifControl send_control(send_interrupts,&command_completions);
        NativeServiceRouter send_services({&sender,&command_completions,&send_interrupts,&send_control});
        store_native_word(ram,0xd400,0); store_native_word(ram,0xd404,0);
        auto register_send=initial(); register_send.pc=0x2224;
        register_send.registers[4]=0x2100; register_send.registers[5]=0xd400;
        check(run_native(nt_iop_generated_run,register_send,ram,1,&send_services).reason==NativeStop::budget,
              "Shared command completion registration failed");
        store_native_word(ram,0xd500,0xd200); store_native_word(ram,0xd504,0x500);
        store_native_word(ram,0xd508,16); store_native_word(ram,0xd50c,0);
        auto raw_send=initial(); raw_send.pc=0x2214; raw_send.registers[4]=0xd500; raw_send.registers[5]=1;
        check(run_native(nt_iop_generated_run,raw_send,ram,1,&send_services).reason==NativeStop::budget && raw_send.registers[2]>0,
              "Shared raw-transfer setup failed");
        store_native_word(ram,0xd318,0x80002140); store_native_word(ram,0xd31c,0xd404);
        const auto callback_send=send_command(28);
        check(callback_send.first.reason==NativeStop::budget && callback_send.second.registers[2]==raw_send.registers[2]+1 &&
              send_command(28).second.registers[2]==0,"Raw and command sends did not share token allocation or peer backpressure");
        auto status_send=initial(); status_send.pc=0x221c; status_send.registers[4]=callback_send.second.registers[2];
        check(run_native(nt_iop_generated_run,status_send,ram,1,&send_services).reason==NativeStop::budget &&
              status_send.registers[2]==0xffffffff,"Command token was not complete through the shared SIF status service");
        foreground=initial(); foreground.pc=0x5000;
        check(send_interrupts.run(nt_iop_generated_run,foreground,ram,32,send_services).reason==NativeStop::unmapped_code &&
              !foreground.executed,"Command completion bypassed the interrupt mask");
        send_interrupts.invoke({9,{},NativeServiceKind::intrman},ram);
        check(send_interrupts.run(nt_iop_generated_run,foreground,ram,1,send_services).reason==NativeStop::budget,
              "Shared command callback did not retain sliced execution");
        auto stop_send=initial(); stop_send.pc=0x228c;
        check(run_native(nt_iop_generated_run,stop_send,ram,1,&send_services).reason==NativeStop::import_request &&
              !stop_send.executed,"SIF teardown discarded an active shared callback");
        check(send_interrupts.run(nt_iop_generated_run,foreground,ram,32,send_services).reason==NativeStop::unmapped_code &&
              foreground.executed==21 && load_native_word(ram,0xd400)==2 && load_native_word(ram,0xd404)==1,
              "Raw/command general and per-command callbacks were duplicated, lost or repeated from a void return");
        command_peer.take();
        store_native_word(ram,0xd318,0x4000);
        const auto before_bad_callback=command_peer_memory;
        const auto before_bad_header=load_native_word(ram,0xd100);
        check(send_command(28).first.reason==NativeStop::unmapped_code && command_peer_memory==before_bad_callback &&
              load_native_word(ram,0xd100)==before_bad_header && !command_peer.take() && !command_completions.take(),
              "Unknown command completion target allowed partial send effects");
        store_native_word(ram,0xd318,0x2140);
        check(send_command(28,true).first.reason==NativeStop::import_request && send_command(29,true).second.registers[2]>0,
              "Completion-send interrupt context handling differs");
        check(run_native(nt_iop_generated_run,stop_send,ram,1,&send_services).reason==NativeStop::budget &&
              stop_send.registers[2]==0 && !command_completions.take() &&
              send_command(29,true).first.reason==NativeStop::import_request && command_peer.take().has_value(),
              "Shared shutdown retained callbacks, accepted new commands or lost committed peer notification");
        {
            NativeSifTransfers boot_transfers(&command_destination);
            NativeSifCommandEndpoint boot_peer(command_destination,0x100,boot_transfers);
            NativeSifCommands boot_commands;
            boot_commands.initialize_system_handlers();
            NativeSifInbound inbound(ram,0xde00);
            NativeCallbackMux boot_sources({&boot_transfers,&inbound});
            NativeInterruptServices boot_interrupts(0xf000,&boot_sources);
            NativeThreadServices boot_threads(boot_interrupts);
            NativeEventServices boot_events(&boot_threads);
            NativeBootServices empty_boot;
            NativeSystemServices boot_system(boot_events,empty_boot,ram);
            NativeSifControl boot_link(boot_interrupts,&boot_transfers);
            NativeSifCommandSession command_boot(boot_events,boot_system,boot_link,&boot_peer);
            NativeSifCommandSession disconnected_boot(boot_events,boot_system,boot_link);
            inbound.connect(boot_commands,command_boot,boot_link);
            NativeServiceRouter boot_services({&command_boot,&boot_threads,&boot_events,&boot_system,&boot_link,&boot_transfers});
            const auto boot_status=boot_system.invoke({41,{},NativeServiceKind::thbase},ram).value;
            const auto boot_call=[&](unsigned ordinal,std::uint32_t a=0,std::uint32_t b=0,NativeServices* service=nullptr) {
                auto state=initial(); state.pc=ordinal==4 ? 0x2454 : ordinal==6 ? 0x245c : ordinal==14 ? 0x246c : 0x2464;
                state.registers[4]=a; state.registers[5]=b;
                const auto stop=run_native(nt_iop_generated_run,state,ram,1,service ? service : &boot_services);
                return std::pair{stop,state};
            };
            check(boot_call(4).first.reason==NativeStop::import_request && boot_link.sub_flags()==0,
                  "Native command initialization invented a ready SIF link");
            boot_link.publish_main(0x100,0x30000);
            check(boot_link.invoke({5,{},NativeServiceKind::sifman},ram).value==0 &&
                  boot_call(4,0,0,&disconnected_boot).first.reason==NativeStop::import_request &&
                  boot_call(4).first.reason==NativeStop::import_request && boot_link.sub_flags()==0x10000,
                  "Command initialization published readiness without a peer or wait continuation");
            auto initializing=initial(); initializing.pc=0x246c; initializing.registers[31]=0xfffffffc;
            const auto initializing_id=boot_threads.admit(initializing);
            check(boot_threads.run(nt_iop_generated_run,ram,32,boot_services).reason==NativeStop::thread_wait &&
                  boot_threads.state(initializing_id).executed==0 && boot_threads.state(initializing_id).pc==0x246c && boot_link.sub_flags()==0x30000,
                  "Command initialization did not publish CMDINIT and suspend through the shared event");
            std::array<std::uint8_t,24> control_packet{};
            store_native_word(control_packet,0,20); store_native_word(control_packet,8,0x80000002);
            store_native_word(control_packet,16,0x40);
            check(command_boot.receive_control(std::span(control_packet).first(20),ram).status==NativeServiceStatus::code_write &&
                  boot_events.invoke({11,{boot_status,0x100,0,0},NativeServiceKind::thevent},ram).value==static_cast<std::uint32_t>(-421) &&
                  boot_threads.run(nt_iop_generated_run,ram,32,boot_services).reason==NativeStop::thread_wait,
                  "Invalid command acknowledgement signalled readiness");
            store_native_word(control_packet,0,16); store_native_word(control_packet,12,1);
            check(command_boot.receive_control(std::span(control_packet).first(16),ram).value==0 &&
                  boot_events.invoke({11,{boot_status,0x800,0,0},NativeServiceKind::thevent},ram).value==0 &&
                  boot_threads.run(nt_iop_generated_run,ram,32,boot_services).reason==NativeStop::thread_wait,
                  "RPC phase acknowledgement incorrectly satisfied command initialization");
            boot_events.invoke({8,{boot_status,~0x800U,0,0},NativeServiceKind::thevent},ram);
            store_native_word(control_packet,0,20); store_native_word(control_packet,12,0); store_native_word(control_packet,16,0x80000200);
            check(inbound.submit(std::span(control_packet).first(20)).value==1 &&
                  boot_threads.run(nt_iop_generated_run,ram,32,boot_services).reason==NativeStop::thread_wait && inbound.busy() &&
                  boot_events.invoke({11,{boot_status,0x100,0,0},NativeServiceKind::thevent},ram).value==static_cast<std::uint32_t>(-421),
                  "Queued command acknowledgement bypassed interrupt masking");
            boot_interrupts.invoke({9,{},NativeServiceKind::intrman},ram);
            check(boot_threads.run(nt_iop_generated_run,ram,0,boot_services).reason==NativeStop::budget && inbound.busy(),
                  "Zero-budget execution delivered an inbound command");
            check(boot_threads.run(nt_iop_generated_run,ram,32,boot_services).reason==NativeStop::thread_wait &&
                  boot_threads.state(initializing_id).pc==0xfffffffc && boot_threads.state(initializing_id).executed==1 &&
                  boot_link.invoke({21,{},NativeServiceKind::sifman},ram).value==0x10000 && boot_call(4).second.registers[2]==0,
                  "Command acknowledgement incorrectly completed RPC initialization");
            auto rpc_initialization=boot_peer.take();
            check(rpc_initialization && rpc_initialization->size()==24 && load_native_word(*rpc_initialization,8)==0x80000001 &&
                  load_native_word(*rpc_initialization,16)==0 && load_native_word(*rpc_initialization,20)==1 &&
                  boot_call(14).first.reason==NativeStop::thread_wait && !boot_peer.take(),
                  "RPC initialization did not publish exactly one readiness command");
            store_native_word(control_packet,0,16); store_native_word(control_packet,12,1);
            check(inbound.submit(std::span(control_packet).first(16)).value==1 &&
                  boot_threads.run(nt_iop_generated_run,ram,32,boot_services).reason==NativeStop::unmapped_code &&
                  boot_threads.state(initializing_id).pc==0xfffffffc && boot_threads.state(initializing_id).registers[2]==0 &&
                  boot_call(14).second.registers[2]==0 && !boot_peer.take(),
                  "RPC peer acknowledgement did not resume initialization idempotently");
            std::array<std::uint8_t,16> boot_outgoing{}; boot_outgoing[0]=16; boot_outgoing[8]=7;
            check(boot_peer.submit(boot_outgoing,{}).value>0 && load_native_word(command_peer_memory,0x208)==7 && boot_peer.take(),
                  "Command acknowledgement did not publish the native peer buffer");
            check(boot_call(7,31,123).second.registers[2]==0 && boot_call(6,31).second.registers[2]==123 &&
                  boot_call(7,32,999).first.reason==NativeStop::import_request && boot_call(6,31).second.registers[2]==123,
                  "Compiled software-register access or index validation differs");
            store_native_word(control_packet,0,24); store_native_word(control_packet,8,0x80000001);
            store_native_word(control_packet,16,31); store_native_word(control_packet,20,456);
            check(command_boot.receive_control(control_packet,ram).value==0 && boot_call(6,31).second.registers[2]==456,
                  "Peer software-register command did not update the shared value");
            store_native_word(control_packet,16,32);
            check(command_boot.receive_control(control_packet,ram).status==NativeServiceStatus::unsupported &&
                  boot_call(6,31).second.registers[2]==456,"Invalid peer register index changed state");
            store_native_word(control_packet,0,20); store_native_word(control_packet,8,0x80000000); store_native_word(control_packet,16,0x300);
            check(command_boot.receive_control(std::span(control_packet).first(20),ram).value==0 &&
                  boot_peer.submit(boot_outgoing,{}).value>0 && load_native_word(command_peer_memory,0x308)==7 && boot_peer.take(),
                  "Peer command-buffer change was not applied");
            check(boot_commands.invoke({8,{0xdc00,4,0,0},NativeServiceKind::sifcmd},ram).value==0 &&
                  boot_commands.invoke({10,{1,0x24c0,0xdd00,0},NativeServiceKind::sifcmd},ram).value==0,
                  "Routed native command setup failed");
            store_native_word(ram,0xdd00,10);
            std::array<std::uint8_t,32> routed_packet{}; routed_packet[0]=32; routed_packet[8]=1; routed_packet[16]=7;
            check(inbound.submit(routed_packet).value==1,"Inbound user command was not accepted");
            routed_packet[16]=99;
            check(inbound.submit(routed_packet).status==NativeServiceStatus::unsupported,"Busy inbound mailbox overwrote its packet");
            foreground=initial(); foreground.pc=0x24d8; foreground.delay_slot=true; foreground.branch_target=0x5000;
            check(boot_interrupts.run(nt_iop_generated_run,foreground,ram,1,boot_services).reason==NativeStop::budget &&
                  inbound.busy() && load_native_word(ram,0xdd00)==10,"Inbound delivery split a branch delay slot");
            check(boot_interrupts.run(nt_iop_generated_run,foreground,ram,1,boot_services).reason==NativeStop::budget &&
                  !inbound.discard(),"Inbound mailbox discarded an active compiled callback");
            check(boot_interrupts.run(nt_iop_generated_run,foreground,ram,32,boot_services).reason==NativeStop::unmapped_code &&
                  foreground.pc==0x5000 && foreground.executed==8 && load_native_word(ram,0xdd00)==17 && !inbound.busy(),
                  "Unified inbound path did not retain its packet or finish the compiled user callback");
            routed_packet[16]=7;
            check(inbound.submit(routed_packet).value==1,"Inbound failure setup did not enqueue");
            store_native_word(ram,0xdc08,0x4000);
            const auto before_inbound_failure=std::vector<std::uint8_t>(ram.begin()+0xde00,ram.begin()+0xde20);
            foreground=initial(); foreground.pc=0x5000;
            const auto inbound_failure=boot_interrupts.run(nt_iop_generated_run,foreground,ram,32,boot_services);
            check(inbound_failure.reason==NativeStop::unmapped_code && inbound_failure.address==0x4000 &&
                  inbound_failure.trap_code==1 && !foreground.executed && inbound.failure() &&
                  std::equal(before_inbound_failure.begin(),before_inbound_failure.end(),ram.begin()+0xde00) &&
                  boot_interrupts.run(nt_iop_generated_run,foreground,ram,32,boot_services).address==0x4000,
                  "Deferred inbound validation failure was lost or changed packet storage");
            check(inbound.discard(),"Explicit inbound error recovery failed");
            store_native_word(ram,0xdc08,0x24c0);
            check(inbound.submit(routed_packet).value==1 &&
                  boot_interrupts.run(nt_iop_generated_run,foreground,ram,32,boot_services).reason==NativeStop::unmapped_code &&
                  load_native_word(ram,0xdd00)==24 && !inbound.failure(),"Inbound source did not recover after explicit discard");
            check(boot_commands.invoke({10,{0x80000001,0x24c0,0xdd04,0},NativeServiceKind::sifcmd},ram).status==NativeServiceStatus::returned,
                  "Native default protocol-handler replacement failed");
            store_native_word(ram,0xdd04,100);
            store_native_word(control_packet,0,24); store_native_word(control_packet,8,0x80000001);
            store_native_word(control_packet,16,31); store_native_word(control_packet,20,999);
            check(inbound.submit(control_packet).value==1,"Default system packet was not accepted");
            foreground=initial(); foreground.pc=0x5000;
            check(boot_interrupts.run(nt_iop_generated_run,foreground,ram,32,boot_services).reason==NativeStop::unmapped_code &&
                  load_native_word(ram,0xdd04)==131 && boot_call(6,31).second.registers[2]==456,
                  "Native protocol dispatch bypassed an individual registered compiled handler");
            check(boot_commands.invoke({11,{0x80000001,0,0,0},NativeServiceKind::sifcmd},ram).status==NativeServiceStatus::returned &&
                  inbound.submit(control_packet).value==1 &&
                  boot_interrupts.run(nt_iop_generated_run,foreground,ram,32,boot_services).reason==NativeStop::unmapped_code &&
                  load_native_word(ram,0xdd04)==131 && boot_call(6,31).second.registers[2]==456,
                  "Removing a default system handler restored built-in behavior or repeated its callback");
            check(boot_commands.invoke({9,{0xdc80,4,0,0},NativeServiceKind::sifcmd},ram).value==0 &&
                  boot_commands.invoke({10,{0x80000001,0x24c0,0xdd00,0},NativeServiceKind::sifcmd},ram).value==0,
                  "Custom inbound system table setup failed");
            store_native_word(control_packet,0,24); store_native_word(control_packet,8,0x80000001);
            store_native_word(control_packet,16,31); store_native_word(control_packet,20,999);
            check(inbound.submit(control_packet).value==1,"Custom system packet was not accepted");
            foreground=initial(); foreground.pc=0x5000;
            check(boot_interrupts.run(nt_iop_generated_run,foreground,ram,32,boot_services).reason==NativeStop::unmapped_code &&
                  load_native_word(ram,0xdd00)==55 && boot_call(6,31).second.registers[2]==456,
                  "Built-in routing bypassed an explicitly installed system handler table");
            boot_events.invoke({5,{boot_status,0,0,0},NativeServiceKind::thevent},ram);
            store_native_word(control_packet,0,20); store_native_word(control_packet,8,0x80000002); store_native_word(control_packet,16,0x400);
            boot_link.publish_main(0x100,0x20000);
            check(command_boot.receive_control(std::span(control_packet).first(20),ram).value==static_cast<std::uint32_t>(-409) &&
                  boot_link.invoke({21,{},NativeServiceKind::sifman},ram).value==0x30000 &&
                  boot_peer.submit(boot_outgoing,{}).value>0 && load_native_word(command_peer_memory,0x408)==0 && boot_peer.take(),
                  "Missing status event partially acknowledged or republished the peer buffer");
            boot_link.invoke({2,{},NativeServiceKind::sifman},ram);
            check(boot_call(4).first.reason==NativeStop::import_request &&
                  command_boot.receive_control(std::span(control_packet).first(20),ram).status==NativeServiceStatus::unsupported,
                  "Stopped SIF link accepted command initialization");
        }
        {
            bool protect_rpc_reply=false;
            std::vector<std::uint8_t> rpc_peer_memory(0x1000,0);
            NativeSifDestination rpc_destination{rpc_peer_memory,[&](std::uint32_t,std::uint32_t) { return !protect_rpc_reply; }};
            NativeSifTransfers rpc_transfers(&rpc_destination);
            NativeSifCommandEndpoint rpc_peer(rpc_destination,0x600,rpc_transfers);
            NativeSifInbound rpc_inbound(ram,0xeb00);
            NativeInterruptServices rpc_interrupts(0xf000,&rpc_inbound);
            NativeThreadServices rpc_workers(rpc_interrupts);
            NativeRpcServers rpc(&rpc_peer,&rpc_workers);
            NativeSifControl rpc_link(rpc_interrupts,&rpc_transfers);
            NativeEventServices rpc_events;
            NativeBootServices rpc_boot;
            NativeSystemServices rpc_system(rpc_events,rpc_boot,ram);
            NativeSifCommandSession rpc_session(rpc_events,rpc_system,rpc_link,&rpc_peer);
            NativeSifCommands rpc_commands(&rpc_peer);
            rpc_inbound.connect(rpc_commands,rpc_session,rpc_link,&rpc);
            rpc_link.publish_main(0x600,0x10000); rpc_link.invoke({5,{},NativeServiceKind::sifman},ram);
            const auto rpc_call=[&](unsigned ordinal,std::uint32_t a,std::uint32_t b=0,std::uint32_t c=0x25c0,std::uint32_t d=0xe100) {
                auto state=initial(); state.pc=ordinal==17 ? 0x2514 : ordinal==19 ? 0x251c : ordinal==24 ? 0x2524 : ordinal==25 ? 0x252c : ordinal==20 ? 0x2534 : 0x253c;
                state.registers[4]=a; state.registers[5]=b; state.registers[6]=c; state.registers[7]=d; state.registers[29]=0xe400;
                const auto stop=run_native(nt_iop_generated_run,state,ram,1,&rpc);
                return std::pair{stop,state};
            };
            std::fill(ram.begin()+0xe000,ram.begin()+0xe044,0x5a);
            store_native_word(ram,0xe410,0); store_native_word(ram,0xe414,0); store_native_word(ram,0xe418,0x8000e300);
            check(rpc_call(19,0xe300,7).second.registers[2]==0 && load_native_word(ram,0xe300)==7 &&
                  load_native_word(ram,0xe308)==0 && load_native_word(ram,0xe314)==0,"Compiled RPC queue creation differs");
            auto registration=initial(); registration.pc=0x2580;
            check(run_native(nt_iop_generated_run,registration,ram,32,&rpc).reason==NativeStop::unmapped_code && registration.pc==0x5000 &&
                  load_native_word(ram,0xe000)==0x1234 && load_native_word(ram,0xe004)==0x25c0 &&
                  load_native_word(ram,0xe008)==0xe100 && load_native_word(ram,0xe040)==0x8000e300 &&
                  load_native_word(ram,0xe308)==0xe000 && load_native_word(ram,0xe00c)==0x5a5a5a5a &&
                  load_native_word(ram,0xe01c)==0x5a5a5a5a,"RPC registration lost ABI fields or overwrote request state");
            auto rpc_function=initial(); rpc_function.pc=0x25c0; rpc_function.registers[4]=37;
            check(run_native(nt_iop_generated_run,rpc_function,ram,8).reason==NativeStop::unmapped_code && rpc_function.registers[2]==42,
                  "RPC registration did not discover its private compiled function");
            check(rpc_call(17,0xa000e500,0x5678).second.registers[2]==0 && load_native_word(ram,0xe038)==0xa000e500 &&
                  load_native_word(ram,0xe538)==0,"RPC server insertion order or aliases differ");
            std::array<std::uint8_t,64> bind_packet{};
            store_native_word(bind_packet,0,64); store_native_word(bind_packet,8,0x80000009);
            store_native_word(bind_packet,20,0x80000400); store_native_word(bind_packet,28,0x80000500);
            store_native_word(bind_packet,32,0x1234);
            const auto pump_bind=[&]() {
                auto state=initial(); state.pc=0x5000;
                return rpc_interrupts.run(nt_iop_generated_run,state,ram,32,rpc);
            };
            check(rpc_inbound.submit(bind_packet).value==1 && pump_bind().reason==NativeStop::unmapped_code &&
                  !rpc_peer.take() && rpc_inbound.busy(),"RPC bind bypassed interrupt masking");
            rpc_interrupts.invoke({9,{},NativeServiceKind::intrman},ram);
            check(pump_bind().reason==NativeStop::unmapped_code && !rpc_inbound.busy(),"RPC bind did not finish through native inbound routing");
            auto bind_reply=rpc_peer.take();
            check(bind_reply && bind_reply->size()==64 && load_native_word(*bind_reply,0)==64 &&
                  load_native_word(*bind_reply,8)==0x80000008 && load_native_word(*bind_reply,20)==0x80000400 &&
                  load_native_word(*bind_reply,28)==0x80000500 && load_native_word(*bind_reply,32)==0x80000009 &&
                  load_native_word(*bind_reply,36)==0xe000 && load_native_word(*bind_reply,40)==0xe100 &&
                  load_native_word(*bind_reply,44)==0,"RPC bind response published incorrect server or correlation fields");
            store_native_word(bind_packet,32,0xffffffff);
            check(rpc_inbound.submit(bind_packet).value==1 && pump_bind().reason==NativeStop::unmapped_code,"Missing-service bind did not respond");
            bind_reply=rpc_peer.take();
            check(bind_reply && load_native_word(*bind_reply,36)==0 && load_native_word(*bind_reply,40)==0,
                  "Missing RPC service fabricated a server");
            store_native_word(bind_packet,32,0x1234);
            std::array<std::uint8_t,16> occupied{}; occupied[0]=16;
            check(rpc_peer.submit(occupied,{}).value>0 && rpc_inbound.submit(bind_packet).value==1 &&
                  pump_bind().reason==NativeStop::unmapped_code && rpc_inbound.busy() && !rpc_inbound.failure(),
                  "RPC peer backpressure dropped or faulted a pending bind");
            store_native_word(ram,0xe008,0xe200);
            check(pump_bind().reason==NativeStop::unmapped_code && rpc_inbound.busy(),"Busy bind was silently consumed on retry");
            const auto occupied_reply=rpc_peer.take();
            check(occupied_reply && occupied_reply->size()==16 && pump_bind().reason==NativeStop::unmapped_code && !rpc_inbound.busy(),
                  "Pending bind failed to retry after peer consumption");
            bind_reply=rpc_peer.take();
            check(bind_reply && load_native_word(*bind_reply,40)==0xe100,"RPC retry changed its prepared reply after descriptor mutation");
            store_native_word(ram,0xe008,0xe100);
            store_native_word(ram,0xe500,0x1234);
            check(rpc_inbound.submit(bind_packet).value==1 && pump_bind().reason==NativeStop::unmapped_code,
                  "Duplicate service-ID bind did not respond");
            bind_reply=rpc_peer.take();
            check(bind_reply && load_native_word(*bind_reply,36)==0xe000,"RPC lookup ignored server registration order");
            store_native_word(ram,0xe500,0x5678);
            protect_rpc_reply=true;
            const auto before_reply_fault=rpc_peer_memory;
            check(rpc_inbound.submit(bind_packet).value==1 && pump_bind().reason==NativeStop::code_write &&
                  rpc_inbound.failure() && rpc_peer_memory==before_reply_fault && !rpc_peer.take(),
                  "Protected RPC reply destination partially committed a response");
            check(rpc_inbound.discard(),"RPC reply failure could not be explicitly discarded"); protect_rpc_reply=false;
            store_native_word(ram,0xe004,0x4000);
            check(rpc_inbound.submit(bind_packet).value==1 && pump_bind().address==0x4000 && !rpc_peer.take(),
                  "RPC binding advertised an uncompiled mutated handler");
            rpc_inbound.discard(); store_native_word(ram,0xe004,0x25c0);
            const auto registered=std::vector<std::uint8_t>(ram.begin()+0xe000,ram.begin()+0xe044);
            check(rpc_call(17,0xe000,1).first.reason==NativeStop::import_request &&
                  rpc_call(17,0xe010,1).first.reason==NativeStop::import_request &&
                  std::equal(registered.begin(),registered.end(),ram.begin()+0xe000),"Duplicate or overlapping RPC descriptors changed existing state");
            check(rpc_call(19,0xe300,99).first.reason==NativeStop::import_request && load_native_word(ram,0xe300)==7,
                  "Duplicate RPC queue reset its live registrations");
            check(rpc_call(17,0xe900,1,0x4000).first.reason==NativeStop::unmapped_code && load_native_word(ram,0xe538)==0,
                  "Unknown RPC callback changed the server chain");
            store_native_word(ram,0xe410,0x4000);
            check(rpc_call(17,0xe900,1).first.reason==NativeStop::unmapped_code,"Unknown RPC secondary callback was accepted");
            store_native_word(ram,0xe410,0);
            check(rpc_call(19,0x24c0,1).first.reason==NativeStop::code_write &&
                  rpc_call(17,0xffe0,1).first.reason==NativeStop::address_fault,
                  "Invalid RPC descriptor storage was accepted");
            check(rpc_call(19,0x8000e800,8).second.registers[2]==0 && load_native_word(ram,0xe314)==0x8000e800,
                  "RPC queue insertion did not maintain public ordering");
            check(rpc_call(24,0xe000,0xe800).second.registers[2]==0 && load_native_word(ram,0xe308)==0xe000,
                  "Removing a server from another queue changed ownership");
            check(rpc_call(24,0x8000e000,0xe300).second.registers[2]==0x8000e000 && load_native_word(ram,0xe308)==0xa000e500 &&
                  rpc_call(24,0xe000,0xe300).second.registers[2]==0,"RPC server removal result or head update differs");
            check(rpc_call(25,0xe300).second.registers[2]==0xe300 && rpc_call(25,0xe300).second.registers[2]==0 &&
                  rpc_call(19,0xe300,7).first.reason==NativeStop::import_request,"Detached RPC queue lost its remaining registrations");
            store_native_word(bind_packet,32,0x5678);
            check(rpc_inbound.submit(bind_packet).value==1 && pump_bind().reason==NativeStop::unmapped_code,
                  "Detached-queue bind did not respond");
            bind_reply=rpc_peer.take();
            check(bind_reply && load_native_word(*bind_reply,36)==0,"Detached RPC queue was still discoverable by its peer");
            check(rpc_call(24,0xe500,0xa000e300).second.registers[2]==0xe500 && load_native_word(ram,0xe308)==0 &&
                  rpc_call(19,0xe300,9).second.registers[2]==0 && load_native_word(ram,0xe814)==0xe300,
                  "Detached queue cleanup or republication differs");
            check(rpc_call(25,0xe300).second.registers[2]==0xe300 && load_native_word(ram,0xe814)==0,
                  "Removing the tail RPC queue left a public link");
            store_native_word(ram,0xe418,0xe800);
            check(rpc_call(17,0xe300,0x9999).second.registers[2]==0 &&
                  rpc_call(24,0xe300,0xe800).second.registers[2]==0xe300,
                  "Removed empty RPC queue retained ownership of reusable descriptor memory");
            NativeServiceRouter rpc_worker_services({&rpc,&rpc_workers});
            auto rpc_worker=initial(); rpc_worker.pc=0x2600; rpc_worker.registers[4]=0xec00; rpc_worker.registers[31]=0xfffffffc;
            const auto rpc_worker_id=rpc_workers.admit(rpc_worker);
            check(rpc_call(19,0xec00,rpc_worker_id).second.registers[2]==0 && rpc_call(20,0xec00).second.registers[2]==0,
                  "RPC worker queue did not start empty");
            store_native_word(ram,0xe418,0xec00);
            check(rpc_call(17,0xec80,0x1111,0x2680,0xee00).second.registers[2]==0 &&
                  rpc_call(17,0xed00,0x2222,0x2680,0xef00).second.registers[2]==0 &&
                  rpc_workers.run(nt_iop_generated_run,ram,32,rpc_worker_services).reason==NativeStop::thread_wait &&
                  rpc_workers.state(rpc_worker_id).pc==0x2610,"Compiled RPC worker did not suspend before request arrival");
            std::array<std::uint8_t,64> call_packet{};
            store_native_word(call_packet,0,64|(4U<<8)); store_native_word(call_packet,4,0x8000ee04);
            store_native_word(call_packet,8,0x8000000a); store_native_word(call_packet,16,6);
            store_native_word(call_packet,20,0x80000400); store_native_word(call_packet,24,77);
            store_native_word(call_packet,28,0x80000500); store_native_word(call_packet,32,0x99);
            store_native_word(call_packet,36,4); store_native_word(call_packet,40,0x700);
            store_native_word(call_packet,44,4); store_native_word(call_packet,48,1); store_native_word(call_packet,52,0x8000ec80);
            std::array<std::uint8_t,4> rpc_payload{1,2,3,4};
            store_native_word(ram,0xee00,0xa5a5a5a5);
            check(rpc_inbound.submit(call_packet,rpc_payload).value==1 && pump_bind().reason==NativeStop::service_request &&
                  load_native_word(ram,0xee00)==0xa5a5a5a5 && load_native_word(ram,0xec0c)==0 &&
                  rpc_workers.state(rpc_worker_id).pc==0x2610,"RPC payload mismatch changed queue, data or worker state");
            rpc_inbound.discard(); store_native_word(call_packet,4,0x8000ee00);
            check(rpc_inbound.submit(call_packet,rpc_payload).value==1,"Valid RPC call was not accepted");
            rpc_payload.fill(9);
            check(pump_bind().reason==NativeStop::unmapped_code && load_native_word(ram,0xee00)==0x04030201 &&
                  load_native_word(ram,0xec0c)==0x8000ec80 && load_native_word(ram,0xec10)==0x8000ec80 &&
                  load_native_word(ram,0xec8c)==4 && load_native_word(ram,0xeca4)==0x99 &&
                  load_native_word(ram,0xec9c)==0x80000500 && load_native_word(ram,0xecb4)==6,
                  "Native RPC admission lost owned payload bytes or public request fields");
            check(rpc_inbound.submit(call_packet,rpc_payload).value==1 && pump_bind().reason==NativeStop::service_request &&
                  load_native_word(ram,0xee00)==0x04030201,"Repeated request overwrote a busy RPC server buffer");
            rpc_inbound.discard();
            store_native_word(call_packet,4,0xef00); store_native_word(call_packet,52,0xed00); store_native_word(call_packet,32,0x55);
            check(rpc_inbound.submit(call_packet,rpc_payload).value==1 && pump_bind().reason==NativeStop::unmapped_code &&
                  load_native_word(ram,0xecbc)==0xed00 && load_native_word(ram,0xec10)==0xed00,
                  "RPC request queue did not retain FIFO order");
            check(rpc_workers.run(nt_iop_generated_run,ram,32,rpc_worker_services).reason==NativeStop::unmapped_code &&
                  rpc_workers.state(rpc_worker_id).pc==0xfffffffc && rpc_workers.state(rpc_worker_id).registers[2]==0x8000ec80 &&
                  load_native_word(ram,0xec04)==1 && load_native_word(ram,0xec0c)==0xed00,
                  "Incoming RPC did not wake its compiled worker or return the first request");
            check(rpc_call(20,0xec00).second.registers[2]==0xed00 && rpc_call(20,0xec00).second.registers[2]==0 &&
                  load_native_word(ram,0xec04)==0 && load_native_word(ram,0xec0c)==0 &&
                  rpc_call(24,0xec80,0xec00).first.reason==NativeStop::import_request &&
                  rpc_call(25,0xec00).first.reason==NativeStop::import_request && !rpc_peer.take(),
                  "RPC dequeue fabricated a reply or removed requests awaiting execution");
            check(rpc_peer.submit(occupied,{}).value>0,"RPC execution backpressure setup failed");
            auto executing=initial(); executing.pc=0x253c; executing.registers[4]=0x8000ec80;
            executing.registers[16]=0x12345678; executing.registers[28]=0x87654321; executing.registers[29]=0xe600;
            check(run_native(nt_iop_generated_run,executing,ram,1,&rpc).reason==NativeStop::budget &&
                  executing.pc==0x2680 && executing.registers[4]==0x99 && executing.registers[5]==0xee00 &&
                  executing.registers[6]==4,"Native RPC execution did not enter its compiled function with request arguments");
            NativeResult execution_stop{};
            for (unsigned slice=0;slice<16;++slice) {
                execution_stop=run_native(nt_iop_generated_run,executing,ram,1,&rpc);
                if (execution_stop.reason!=NativeStop::budget) break;
            }
            check(execution_stop.reason==NativeStop::thread_wait && executing.pc==native_service_return && executing.executed==9 &&
                  load_native_word(ram,0xee00)==0x0403029e && rpc_call(24,0xec80,0xec00).first.reason==NativeStop::import_request,
                  "RPC callback did not complete once before waiting for its reply endpoint");
            store_native_word(ram,0xee00,0xdeadc0de);
            check(run_native(nt_iop_generated_run,executing,ram,32,&rpc).reason==NativeStop::thread_wait && executing.executed==9 &&
                  load_native_word(ram,0xee00)==0xdeadc0de,"RPC backpressure reran the handler or consumed execution budget");
            rpc_peer.take();
            check(run_native(nt_iop_generated_run,executing,ram,32,&rpc).reason==NativeStop::unmapped_code && executing.pc==0x5000 &&
                  executing.executed==9 && executing.registers[2]==0 && executing.registers[4]==0x8000ec80 &&
                  executing.registers[16]==0x12345678 && executing.registers[28]==0x87654321 && executing.registers[29]==0xe600 &&
                  executing.registers[31]==0x5000,"RPC reply completion did not restore the native caller continuation");
            auto call_reply=rpc_peer.take();
            check(call_reply && load_native_word(*call_reply,0)==(64U|(4U<<8)) && load_native_word(*call_reply,4)==0x700 &&
                  load_native_word(*call_reply,8)==0x80000008 && load_native_word(*call_reply,28)==0x80000500 &&
                  load_native_word(*call_reply,32)==0x8000000a && load_native_word(rpc_peer_memory,0x700)==0x0403029e &&
                  rpc_call(24,0xec80,0xec00).second.registers[2]==0xec80,
                  "RPC reply lost its result snapshot, client identity or completed server ownership");
            store_native_word(ram,0xed30,2);
            check(rpc_call(21,0xed00).first.reason==NativeStop::import_request && load_native_word(ram,0xef00)==0x09090909,
                  "Invalid RPC reply mode executed a handler");
            store_native_word(ram,0xed30,0); store_native_word(ram,0xed04,0x4000);
            check(rpc_call(21,0xed00).first.reason==NativeStop::unmapped_code && load_native_word(ram,0xef00)==0x09090909,
                  "RPC execution trusted an uncompiled replacement function");
            store_native_word(ram,0xed04,0x2680);
            auto second_execution=initial(); second_execution.pc=0x253c; second_execution.registers[4]=0xed00;
            check(rpc_peer.submit(occupied,{}).value>0,"Polling reply occupied-mailbox setup failed");
            store_native_word(rpc_peer_memory,0x410,0xffffffff); store_native_word(rpc_peer_memory,0x418,77);
            const auto before_polling_reply=rpc_peer_memory;
            protect_rpc_reply=true;
            check(run_native(nt_iop_generated_run,second_execution,ram,32,&rpc).reason==NativeStop::code_write &&
                  second_execution.pc==native_service_return && second_execution.executed==9 && load_native_word(ram,0xef00)==0x09090962 &&
                  rpc_peer_memory==before_polling_reply,"Protected polling reply partially committed result or completion");
            protect_rpc_reply=false;
            check(run_native(nt_iop_generated_run,second_execution,ram,32,&rpc).reason==NativeStop::unmapped_code &&
                  second_execution.pc==0x5000 && second_execution.executed==9 &&
                  load_native_word(rpc_peer_memory,0x700)==0x09090962 &&
                  load_native_word(rpc_peer_memory,0x410)==0 && load_native_word(rpc_peer_memory,0x418)==0 &&
                  load_native_word(rpc_peer_memory,0x41c)==0x80000500 && load_native_word(rpc_peer_memory,0x420)==0x8000000a &&
                  rpc_peer.take()==std::optional<std::vector<std::uint8_t>>(std::vector<std::uint8_t>(occupied.begin(),occupied.end())) && !rpc_peer.take() &&
                  rpc_call(24,0xed00,0xec00).second.registers[2]==0xed00 && rpc_call(25,0xec00).second.registers[2]==0xec00,
                  "Retrying a validated RPC reply reran the handler or retained busy ownership");
            auto loop_worker=initial(); loop_worker.pc=0x2544; loop_worker.registers[4]=0xec00;
            loop_worker.registers[31]=0xfffffffc;
            const auto loop_id=rpc_workers.admit(loop_worker);
            check(rpc_call(19,0xec00,loop_id).second.registers[2]==0 &&
                  rpc_call(17,0xec80,0x1111,0x2680,0xee00).second.registers[2]==0 &&
                  rpc_call(17,0xed00,0x2222,0x2680,0xef00).second.registers[2]==0 &&
                  rpc_workers.run(nt_iop_generated_run,ram,32,rpc_worker_services).reason==NativeStop::thread_wait &&
                  rpc_workers.state(loop_id).pc==0x2544 && rpc_workers.state(loop_id).executed==0,
                  "Native RPC loop did not sleep at its import while the queue was empty");
            store_native_word(call_packet,4,0xee00); store_native_word(call_packet,52,0xec80);
            store_native_word(call_packet,32,1); store_native_word(call_packet,40,0x700);
            store_native_word(call_packet,48,1); rpc_payload={1,0,0,0};
            check(rpc.receive_call(call_packet,rpc_payload,ram).status==NativeServiceStatus::returned,
                  "RPC loop first request was not admitted");
            store_native_word(call_packet,4,0xef00); store_native_word(call_packet,52,0xed00);
            store_native_word(call_packet,32,2); store_native_word(call_packet,40,0x704);
            store_native_word(call_packet,48,0); store_native_word(call_packet,20,0x440);
            check(rpc.receive_call(call_packet,rpc_payload,ram).status==NativeServiceStatus::returned,
                  "RPC loop second request was not admitted");
            store_native_word(ram,0xec84,0x4000);
            check(rpc_workers.run(nt_iop_generated_run,ram,32,rpc_worker_services).reason==NativeStop::unmapped_code &&
                  load_native_word(ram,0xec0c)==0xec80 && load_native_word(ram,0xee00)==1 &&
                  rpc_workers.state(loop_id).executed==0,
                  "RPC loop consumed a request whose handler was not compiled");
            store_native_word(ram,0xec84,0x2680);
            check(rpc_workers.run(nt_iop_generated_run,ram,32,rpc_worker_services).reason==NativeStop::thread_wait &&
                  rpc_workers.state(loop_id).pc==0x2544 && rpc_workers.state(loop_id).registers[4]==0xec00 &&
                  load_native_word(ram,0xec04)==0 && load_native_word(ram,0xec0c)==0 &&
                  load_native_word(rpc_peer_memory,0x700)==6 && load_native_word(rpc_peer_memory,0x704)==7 &&
                  load_native_word(rpc_peer_memory,0x460)==0x8000000a && rpc_peer.take(),
                  "RPC loop failed to drain both reply modes and return to sleep");
            const auto loop_count=rpc_workers.state(loop_id).executed;
            check(rpc.receive_call(call_packet,rpc_payload,ram).status==NativeServiceStatus::returned &&
                  rpc_workers.run(nt_iop_generated_run,ram,32,rpc_worker_services).reason==NativeStop::thread_wait &&
                  rpc_workers.state(loop_id).pc==0x2544 && rpc_workers.state(loop_id).executed==loop_count+9 &&
                  !rpc_peer.take() && rpc_call(24,0xec80,0xec00).second.registers[2]==0xec80 &&
                  rpc_call(24,0xed00,0xec00).second.registers[2]==0xed00,
                  "RPC loop failed to wake for a reused server or executed a request more than once");
            NativeRpcDataServices rpc_data(&rpc_peer),unconnected_data;
            const auto read_peer=[&](std::uint32_t source,std::uint32_t destination,std::uint32_t size,NativeServices* service=nullptr) {
                auto cpu=initial(); cpu.pc=0x254c; cpu.registers[4]=0x8000e900; cpu.registers[5]=source;
                cpu.registers[6]=destination; cpu.registers[7]=size; cpu.registers[29]=0xe400;
                const auto stop=run_native(nt_iop_generated_run,cpu,ram,1,service ? service : &rpc_data);
                return std::pair{stop,cpu};
            };
            for (unsigned index=0;index<19;++index) rpc_peer_memory[0x801+index]=static_cast<std::uint8_t>(index*13);
            std::fill(ram.begin()+0xe900,ram.begin()+0xe91c,0xa5);
            store_native_word(ram,0xe410,0);
            check(rpc_peer.submit(occupied,{}).value>0 && read_peer(0x80000801,0xa000e981,19).first.reason==NativeStop::budget &&
                  std::equal(rpc_peer_memory.begin()+0x801,rpc_peer_memory.begin()+0x814,ram.begin()+0xe981) &&
                  load_native_word(ram,0xe900)==0 && load_native_word(ram,0xe904)==1 && load_native_word(ram,0xe908)==0xffffffff &&
                  load_native_word(ram,0xe90c)==0xa5a5a5a5 && load_native_word(ram,0xe918)==0xa5a5a5a5 && rpc_peer.take(),
                  "Native peer read lost bytes, descriptor completion or an unrelated command");
            store_native_word(ram,0xe410,1);
            check(read_peer(0x801,0xe981,19).first.reason==NativeStop::budget && load_native_word(ram,0xe904)==2 && !rpc_peer.take(),
                  "NOWAIT peer read did not complete without a fabricated command packet");
            const auto before_read_fault=ram;
            check(read_peer(0xff8,0xe981,19).first.reason==NativeStop::address_fault &&
                  read_peer(0x801,0x1001,19).first.reason==NativeStop::code_write &&
                  read_peer(0x801,0xe904,19).first.reason==NativeStop::import_request &&
                  read_peer(0x801,0xfff8,19).first.reason==NativeStop::address_fault &&
                  read_peer(0x801,0xe981,19,&unconnected_data).first.reason==NativeStop::import_request && ram==before_read_fault,
                  "Rejected peer read changed data or completion metadata");
            store_native_word(ram,0xe410,2);
            check(read_peer(0x801,0xe981,19).first.reason==NativeStop::import_request,
                  "Unknown peer-read mode was silently accepted");
            store_native_word(ram,0xe410,0);
            check(read_peer(0xffffffff,0xffffffff,0).first.reason==NativeStop::budget && load_native_word(ram,0xe904)==3,
                  "Zero-byte peer read dereferenced unused pointers or lost completion");
            rpc_transfers.stop();
            check(read_peer(0x801,0xe981,19).first.reason==NativeStop::import_request,
                  "Stopped native peer accepted a read");
            auto irq_registration=initial(); irq_registration.pc=0x251c; irq_registration.registers[4]=0xea00; irq_registration.interrupt_depth=1;
            check(run_native(nt_iop_generated_run,irq_registration,ram,1,&rpc).reason==NativeStop::import_request && !irq_registration.executed,
                  "RPC queue registration ignored interrupt context");
        }
        for (const auto diagnostic_entry : {0x2314U,0x2354U}) {
            std::vector<std::string> diagnostic_output;
            NativeStdioServices diagnostics([&](std::string_view text) { diagnostic_output.emplace_back(text); return true; });
            const auto diagnostic_string=[&](std::uint32_t address,std::string_view text) {
                std::copy(text.begin(),text.end(),ram.begin()+address); ram[address+text.size()]=0;
            };
            diagnostic_string(0xca00,"%d %#08x %.3s %c %lu %.*s %%"); diagnostic_string(0xcb00,"abcdef");
            auto print=initial(); print.pc=diagnostic_entry; print.registers[4]=0xca00;
            print.registers[5]=0x80000000; print.registers[6]=42; print.registers[7]=0xcb00; print.registers[29]=0xcb80;
            store_native_word(ram,0xcb90,'Q'); store_native_word(ram,0xcb94,0xffffffff);
            store_native_word(ram,0xcb98,3); store_native_word(ram,0xcb9c,0xcb00);
            const std::string expected_diagnostic="-2147483648 0x00002a abc Q 4294967295 abc %";
            check(run_native(nt_iop_generated_run,print,ram,1,&diagnostics).reason==NativeStop::budget &&
                  print.registers[2]==expected_diagnostic.size() && diagnostic_output.size()==1 && diagnostic_output.back()==expected_diagnostic,
                  "Compiled printf did not preserve signed, prefix, precision or stack-vararg formatting");
            diagnostic_string(0xca00,"[%*.*s] [%#.0o] [%+.4d]");
            print=initial(); print.pc=diagnostic_entry; print.registers[4]=0xca00; print.registers[5]=static_cast<std::uint32_t>(-6);
            print.registers[6]=3; print.registers[7]=0xcb00; print.registers[29]=0xcb80;
            store_native_word(ram,0xcb90,0); store_native_word(ram,0xcb94,12);
            check(run_native(nt_iop_generated_run,print,ram,1,&diagnostics).reason==NativeStop::budget &&
                  diagnostic_output.back()=="[abc   ] [0] [+0012]","Dynamic printf width/precision or zero-value alternate formatting differs");
            diagnostic_string(0xca00,"%p [%12p] [%-12p] [%012p]");
            print=initial(); print.pc=diagnostic_entry; print.registers[4]=0xca00;
            print.registers[5]=0xffffffff; print.registers[6]=0; print.registers[7]=0x80001234; print.registers[29]=0xcb80;
            store_native_word(ram,0xcb90,0xab);
            const std::string pointer_text="0xffffffff [  0x00000000] [0x80001234  ] [0x00000000ab]";
            check(run_native(nt_iop_generated_run,print,ram,1,&diagnostics).reason==NativeStop::budget &&
                print.registers[2]==pointer_text.size() && diagnostic_output.back()==pointer_text,
                "Compiled pointer formatting dereferenced a value or lost width, aliases or stack arguments");
            const auto prior_messages=diagnostic_output.size();
            diagnostic_string(0xca00,"must not print %n");
            print=initial(); print.pc=diagnostic_entry; print.registers[4]=0xca00;
            const auto rejected_format=run_native(nt_iop_generated_run,print,ram,1,&diagnostics);
            check(rejected_format.reason==NativeStop::import_request && rejected_format.address==0xca00 && !print.executed &&
                  diagnostic_output.size()==prior_messages,"Unsupported printf emitted partial text or retired");
            diagnostic_string(0xca00,"must not print %s"); ram[0xffff]='X';
            print=initial(); print.pc=diagnostic_entry; print.registers[4]=0xca00; print.registers[5]=0xffff;
            check(run_native(nt_iop_generated_run,print,ram,1,&diagnostics).reason==NativeStop::address_fault && !print.executed &&
                  diagnostic_output.size()==prior_messages,"Faulting printf string emitted partial output");
            diagnostic_string(0xca00,"%u%u%u%u");
            print=initial(); print.pc=diagnostic_entry; print.registers[4]=0xca00; print.registers[29]=0xfffffff0;
            check(run_native(nt_iop_generated_run,print,ram,1,&diagnostics).reason==NativeStop::address_fault && !print.executed &&
                  diagnostic_output.size()==prior_messages,"Printf vararg address wrapped or committed partial output");
            NativeStdioServices failed_output([](std::string_view) { return false; });
            diagnostic_string(0xca00,"output failure"); print=initial(); print.pc=diagnostic_entry; print.registers[4]=0xca00;
            check(run_native(nt_iop_generated_run,print,ram,1,&failed_output).reason==NativeStop::budget && print.registers[2]==0xffffffff,
                  "Printf did not report host output failure");
            if (diagnostic_entry==0x2354) {
                const auto before=diagnostic_output.size();
                print=initial(); print.pc=diagnostic_entry; print.registers[4]=0;
                check(run_native(nt_iop_generated_run,print,ram,1,&diagnostics).reason==NativeStop::import_request &&
                      !print.executed && diagnostic_output.size()==before,
                      "Null Kprintf control request was treated as diagnostic text");
            }
        }
        // Compiled import calls share a host allocator across budget boundaries.
        NativeHeapServices heap(0x10000,0x6000,0x9000);
        const auto heap_call=[&](unsigned ordinal,std::uint32_t a=0,std::uint32_t b=0,std::uint32_t c=0) {
            auto state=initial(); state.pc=0x12c8+(ordinal-4)*8;
            state.registers[4]=a; state.registers[5]=b; state.registers[6]=c;
            const auto stop=run_native(nt_iop_generated_run,state,ram,1,&heap);
            check(stop.reason==NativeStop::budget && state.executed==1 && state.pc==0x5000,
                  "Compiled allocation service did not retire and return once");
            return state.registers[2];
        };
        check(heap_call(6)==0x10000 && heap_call(7)==0x3000 && heap_call(8)==0x3000,"Initial native arena queries differ");
        auto allocation=initial(); allocation.pc=0x1308; allocation.registers[4]=0; allocation.registers[5]=257;
        NativeResult allocation_stop{};
        do { allocation_stop=run_native(nt_iop_generated_run,allocation,ram,1,&heap); }
        while (allocation_stop.reason==NativeStop::budget);
        check(allocation_stop.reason==NativeStop::unmapped_code && allocation.executed==7 && allocation.registers[2]==0x6000,
              "Compiled caller did not allocate and resume through native service");
        check(heap_call(4,1,1)==0x8f00 && heap_call(4,2,256,0x7000)==0x7000,
              "Last-fit or fixed-address allocation differs");
        check(heap_call(9,0x60ff)==0x6000 && heap_call(10,0x60ff)==512 &&
              heap_call(9,0x6300)==0x80006200 && heap_call(10,0x6300)==0x80000e00,
              "Allocated/free extent query flags or rounded sizes differ");
        // Use allocated memory through compiled native memset, then preserve it on free.
        auto fill=initial(); fill.pc=0x124c; fill.registers[4]=0x6000; fill.registers[5]=0x5a; fill.registers[6]=512;
        check(nt_iop_generated_run(fill,ram,1).reason==NativeStop::budget && ram[0x61ff]==0x5a,"Allocated storage is not usable by native code");
        const auto free_before=heap_call(8);
        for (const auto size : {0U,0xffffffffU,0x10001U,0x10000U}) check(heap_call(4,0,size)==0,"Invalid/exhausted allocation succeeded");
        check(heap_call(4,3,256)==0 && heap_call(4,2,256,0x7001)==0 && heap_call(4,2,256,0x7000)==0 &&
              heap_call(4,2,256,0x1000)==0 && heap_call(4,2,256,0x9000)==0 && heap_call(8)==free_before,
              "Rejected allocation changed arena or overlapped protected regions");
        check(heap_call(5,0x6001)==0xffffffff && heap_call(5,0)==0xffffffff && heap_call(5,0x9000)==0xffffffff,
              "Invalid or reserved free succeeded");
        check(heap_call(5,0x6000)==0 && heap_call(5,0x6000)==0xffffffff && heap_call(5,0x7000)==0 && heap_call(5,0x8f00)==0 &&
              heap_call(7)==0x3000 && heap_call(8)==0x3000 && ram[0x6000]==0x5a,
              "Free/coalescing lost capacity or unexpectedly cleared payload");
        check(heap_call(4,0,0x3000)==0x6000 && heap_call(4,0,1)==0 && heap_call(5,0x6000)==0,
              "Whole-arena exhaustion and recovery differ");
        auto unbound=initial(); unbound.pc=0x12c8;
        check(run_native(nt_iop_generated_run,unbound,ram,1).reason==NativeStop::import_request && !unbound.executed,
              "Missing import provider silently succeeded");
        unbound.delay_slot=true;
        check(run_native(nt_iop_generated_run,unbound,ram,1,&heap).reason==NativeStop::branch_in_delay && !unbound.executed,
              "Import executed in a delay slot");
        auto pending=initial(); pending.pc=0x12c8; pending.registers[4]=0; pending.registers[5]=0;
        pending.load_register=5; pending.load_value=256;
        check(run_native(nt_iop_generated_run,pending,ram,1,&heap).reason==NativeStop::budget &&
              pending.registers[2]==0x6000 && !pending.load_register,"Native import ignored pending argument load");
        check(heap_call(5,0x6000)==0,"Pending-load allocation could not be freed");
        auto zero=initial(); zero.pc=0x12c8;
        check(run_native(nt_iop_generated_run,zero,ram,0,&heap).reason==NativeStop::budget && heap_call(8)==0x3000,
              "Zero budget invoked allocation");
        auto pending_return=initial(); pending_return.pc=0x12d8; pending_return.load_register=31; pending_return.load_value=0x5100;
        check(run_native(nt_iop_generated_run,pending_return,ram,1,&heap).reason==NativeStop::budget && pending_return.pc==0x5100,
              "Native import ignored pending return-address load");
        check(heap_call(9,0x10000)==0xffffffff && heap_call(10,0xffffffff)==0xffffffff,
              "Out-of-arena block query succeeded");
        const NativeServiceRequest query{8,{},NativeServiceKind::sysmem};
        check(heap.invoke(query,std::span<std::uint8_t>(ram).first(0x8000)).status==NativeServiceStatus::address_fault &&
              heap_call(8)==0x3000 && heap.invoke({8,{}},ram).status==NativeServiceStatus::unsupported,
              "Invalid arena or syscall/import collision changed allocator state");
        NativeHeapServices full(0x10000,0x9000,0x9000);
        check(full.invoke(query,ram).value==0 && full.invoke({4,{0,256,0,0},NativeServiceKind::sysmem},ram).value==0,
              "Fully reserved arena exposed free memory");
        for (const auto alias : {0U,0x80000000U,0xa0000000U}) {
            auto protected_stub=initial(alias); protected_stub.pc=alias|0x124c;
            protected_stub.registers[4]=alias|0x1250; protected_stub.registers[5]=0xff; protected_stub.registers[6]=4;
            const auto stop=nt_iop_generated_run(protected_stub,ram,1);
            check(stop.reason==NativeStop::code_write && !protected_stub.executed && load_native_word(ram,0x1250)==0,
                  "Native fill overwrote the second word of a replaced import stub");
            protected_stub.pc=alias|0x1250;
            const auto invalid_entry=nt_iop_generated_run(protected_stub,ram,1);
            check(invalid_entry.reason==NativeStop::unmapped_code && !protected_stub.executed,
                  "Protected import metadata became an executable native entry");
        }
        for (const auto alias : {0U,0x80000000U,0xa0000000U}) for (const auto entry : {0x1284U,0x129cU}) {
            for (unsigned n=0;n<32;++n) { ram[0x4200+n]=static_cast<std::uint8_t>(n*7); ram[0x4300+n]=0; }
            auto copy=initial(alias); copy.pc=alias|entry;
            copy.registers[4]=alias|0x4300; copy.registers[5]=0xa0004200; copy.registers[6]=32;
            NativeResult stop{};
            do { stop=nt_iop_generated_run(copy,ram,1); } while (stop.reason==NativeStop::budget);
            check(stop.reason==NativeStop::unmapped_code && copy.executed==7 && copy.registers[2]==(alias|0x4300),
                  "Native copy import did not return through its caller");
            for (unsigned n=0;n<32;++n) check(ram[0x4300+n]==static_cast<std::uint8_t>(n*7),"Native copy payload differs");
            const auto stub=entry==0x1284 ? 0x1254U : 0x125cU;
            copy=initial(alias); copy.pc=alias|stub; copy.registers[4]=alias|0x4300;
            copy.registers[5]=0xffff; copy.registers[6]=2;
            ram[0x4300]=0x87;
            stop=nt_iop_generated_run(copy,ram,1);
            check(stop.reason==NativeStop::address_fault && !copy.executed && ram[0x4300]==0x87,
                  "Invalid native copy source partially changed destination");
            copy.registers[4]=alias|0x1000; copy.registers[5]=alias|0x4200; copy.registers[6]=4;
            stop=nt_iop_generated_run(copy,ram,1);
            check(stop.reason==NativeStop::code_write && !copy.executed && ram[0x1000]==0,
                  "Native copy modified compiled code");
            copy.registers[4]=0xffffffff; copy.registers[5]=0xffffffff; copy.registers[6]=0;
            stop=nt_iop_generated_run(copy,ram,1);
            check(stop.reason==NativeStop::budget && copy.executed==1 && copy.registers[2]==0xffffffff,
                  "Empty native copy accessed its pointers");
        }
        for (const auto destination : {0x4400U,0x4403U,0x4406U}) {
            for (unsigned n=0;n<32;++n) ram[0x4400+n]=static_cast<std::uint8_t>(n+1);
            const auto before=ram;
            auto move=initial(); move.pc=0x125c; move.registers[4]=destination;
            move.registers[5]=0xffffffff; move.load_register=5; move.load_value=0xa0004403; move.registers[6]=16;
            auto stop=nt_iop_generated_run(move,ram,1);
            check(stop.reason==NativeStop::budget && move.executed==1 && !move.load_register,
                  "Native memmove did not settle its pending source argument");
            for (unsigned n=0;n<32;++n) {
                const auto address=0x4400+n;
                const auto expected=address>=destination && address<destination+16 ? before[0x4403+address-destination] : before[address];
                check(ram[address]==expected,"Overlapping native memmove corrupted data");
            }
            ram=before;
            move=initial(); move.pc=0x1254; move.registers[4]=destination; move.registers[5]=0xa0004403; move.registers[6]=16;
            stop=nt_iop_generated_run(move,ram,1);
            check(stop.reason==NativeStop::address_fault && !move.executed && ram==before,
                  "Overlapping native memcpy was not rejected atomically");
        }
        for (const auto alias : {0U,0x80000000U,0xa0000000U}) for (const auto budget : {1U,20U}) {
            auto filled=initial(alias); filled.pc=alias|0x126c;
            filled.registers[4]=alias|0x4001; filled.registers[5]=0x1ab; filled.registers[6]=13;
            ram[0x4000]=0x35; ram[0x400e]=0x56;
            NativeResult stop{};
            do { stop=nt_iop_generated_run(filled,ram,budget); } while (stop.reason==NativeStop::budget);
            check(stop.reason==NativeStop::unmapped_code && filled.executed==7 && filled.registers[2]==(alias|0x4001) &&
                  ram[0x4000]==0x35 && ram[0x400e]==0x56,"Native memset import return, bounds or budget differs");
            for (unsigned offset=0x4001;offset<0x400e;++offset) check(ram[offset]==0xab,"Native memset byte differs");
            filled=initial(alias); filled.pc=alias|0x124c; filled.registers[4]=alias|0x1000;
            filled.registers[5]=0xff; filled.registers[6]=8;
            stop=nt_iop_generated_run(filled,ram,1);
            check(stop.reason==NativeStop::code_write && !filled.executed && ram[0x1000]==0,
                  "Native memset modified converted code");
            filled.registers[4]=alias|0xffff; filled.registers[6]=2;
            ram[0xffff]=0x42;
            stop=nt_iop_generated_run(filled,ram,1);
            check(stop.reason==NativeStop::address_fault && !filled.executed && ram[0xffff]==0x42,
                  "Native memset performed a partial out-of-bounds write");
            filled.registers[4]=0xffffffff; filled.registers[6]=0;
            stop=nt_iop_generated_run(filled,ram,1);
            check(stop.reason==NativeStop::budget && filled.executed==1 && filled.registers[2]==0xffffffff,
                  "Empty native memset accessed its pointer");
        }
        constexpr std::uint32_t arithmetic_values[]{0,1,0xffffffff,0x7fffffff,0x80000000};
        for (const auto entry : {0x11e4U,0x11f0U,0x11fcU,0x1208U})
            for (const auto a : arithmetic_values) for (const auto b : arithmetic_values) {
                const auto left=std::int64_t(std::bit_cast<std::int32_t>(a));
                const auto right=entry==0x11fc ? -1LL : std::int64_t(std::bit_cast<std::int32_t>(b));
                const auto wide=entry==0x11f0 ? left-right : left+right;
                const bool overflow=wide<-2147483648LL || wide>2147483647LL;
                auto checked=initial(); checked.pc=entry; checked.registers[4]=a; checked.registers[5]=b;
                checked.load_register=12; checked.load_value=123; checked.registers[12]=456;
                auto stop=nt_iop_generated_run(checked,ram,1);
                if (overflow) {
                    check(stop.reason==NativeStop::overflow && !checked.executed && checked.pc==entry &&
                          checked.registers[2]==99 && checked.load_register==12 && checked.registers[12]==456,
                          "Checked arithmetic overflow retired state or ignored a zero destination");
                    checked.delay_slot=true; checked.branch_target=0x5000;
                    stop=nt_iop_generated_run(checked,ram,1);
                    check(stop.reason==NativeStop::overflow && checked.delay_slot && checked.branch_target==0x5000 &&
                          !checked.executed,"Delay-slot arithmetic overflow lost branch state");
                } else {
                    check(stop.reason==NativeStop::budget && checked.executed==1 && checked.registers[12]==123 &&
                          checked.registers[2]==(entry==0x1208 ? 99U : static_cast<std::uint32_t>(wide)),
                          "Non-overflow arithmetic result or load retirement differs");
                    stop=nt_iop_generated_run(checked,ram,10);
                    check(stop.reason==NativeStop::unmapped_code && checked.executed==3 && checked.pc==0x5000,
                          "Checked arithmetic failed to return through compiled continuation");
                }
            }
        for (const auto alias : {0U,0x80000000U,0xa0000000U}) for (const auto budget : {1U,3U,64U}) {
            for (const auto entry : {0x1190U,0x11ccU}) {
                auto indirect=initial(alias); indirect.pc=alias|entry;
                NativeResult stop{};
                do { stop=nt_iop_generated_run(indirect,ram,budget); } while (stop.reason==NativeStop::budget);
                check(stop.reason==NativeStop::unmapped_code && indirect.pc==0x5000 &&
                      indirect.executed==(entry==0x1190 ? 11U : 6U) && indirect.registers[2]==(entry==0x1190 ? 13U : 10U),
                      "Statically resolved register call/jump did not preserve control flow");
            }
            auto unexpected=initial(alias); unexpected.pc=alias|0x119c; unexpected.registers[8]=0x1000;
            const auto stop=nt_iop_generated_run(unexpected,ram,1);
            check(stop.reason==NativeStop::unmapped_code && unexpected.pc==(alias|0x119c) &&
                  stop.address==0x1000 && !unexpected.executed && unexpected.registers[31]==0x5000,
                  "Unexpected indirect target escaped its compile-time proof");
        }
        auto trapped_callback=initial(); trapped_callback.pc=0x138c;
        const auto trapped_stop=nt_iop_generated_run(trapped_callback,ram,10);
        check(trapped_stop.reason==NativeStop::guest_break && trapped_stop.trap_code==7 &&
              trapped_callback.executed==3 && trapped_callback.delay_slot && trapped_callback.branch_target==0x1360,
              "Indirect target proof was lost before a trapping delay slot");
        for (const auto alias : {0U,0x80000000U,0xa0000000U}) for (const auto budget : {1U,2U,100U}) {
            for (const auto choice : {0U,1U}) {
                auto callback=initial(alias); callback.pc=alias|0x1320; callback.registers[4]=choice;
                NativeResult stop{};
                do { stop=nt_iop_generated_run(callback,ram,budget); } while (stop.reason==NativeStop::budget);
                check(stop.reason==NativeStop::unmapped_code && callback.pc==0x5000 &&
                      callback.executed==(choice ? 15U : 13U) && callback.registers[2]==(choice ? 12U : 10U),
                      "Branch-selected indirect call lost a target, delay slot or continuation");
            }
            for (const auto target : {0x1360U,0x1380U,0x1384U,0x4000U,0x1361U}) {
                auto guarded=initial(alias); guarded.pc=alias|0x1348; guarded.registers[8]=alias|target;
                const auto stop=nt_iop_generated_run(guarded,ram,1);
                const bool valid=target==0x1360 || target==0x1380;
                check(valid ? stop.reason==NativeStop::budget && guarded.executed==1 && guarded.delay_slot &&
                                  guarded.branch_target==(alias|target) && guarded.registers[31]==(alias|0x1350)
                            : stop.reason==NativeStop::unmapped_code && !guarded.executed && !guarded.delay_slot &&
                                  stop.address==(alias|target) && guarded.registers[31]==0x5000,
                      "Indirect call escaped its finite target proof or rejected a valid alias");
            }
        }
        {
            std::vector<std::uint8_t> dma_ram(0x11000);
            BootstrapDevices devices;
            NativeDmacmanServices dma(devices);
            for (const auto alias : {0U,0x80000000U,0xa0000000U}) {
                auto sequence=initial(alias); sequence.pc=alias|0x4860;
                NativeResult stop{};
                do { stop=run_native(nt_iop_generated_run,sequence,dma_ram,1,&dma); } while (stop.reason==NativeStop::budget);
                const auto priority=devices.read(0x1f801570,4);
                check(stop.reason==NativeStop::unmapped_code && sequence.pc==0x5000 && sequence.executed==20 &&
                      sequence.registers[2]==0x12345d78 && priority && *priority==0x12345d78,
                      "Compiled dmacman priority/enable sequence did not update DPCR2");
            }
            const auto priority_before=devices.read(0x1f801570,4);
            const auto expect_stop=[&](std::uint32_t entry,std::uint32_t stub,std::uint32_t ordinal,unsigned executed) {
                auto cpu=initial(); cpu.pc=entry;
                const auto stop=run_native(nt_iop_generated_run,cpu,dma_ram,16,&dma);
                const auto priority=devices.read(0x1f801570,4);
                check(stop.reason==NativeStop::import_request && stop.pc==stub && stop.trap_code==(0x100000U|ordinal) &&
                      cpu.executed==executed && priority && priority_before && *priority==*priority_before,
                      "DMA stop reported success or changed channel priority");
            };
            expect_stop(0x48a0,0x4834,28,2); expect_stop(0x48b0,0x483c,32,2);
            expect_stop(0x48c0,0x481c,33,2); expect_stop(0x48d0,0x4844,8,4);
            const auto channel=DmaController::channel_register(3,8);
            const auto chcr=channel ? devices.read(*channel,4) : std::optional<std::uint32_t>{};
            check(chcr && *chcr==0,"Rejected CHCR START wrote the channel register");
            check(dma.invoke({28,{3},NativeServiceKind::dmacman},dma_ram).status==NativeServiceStatus::unavailable &&
                  dma.invoke({32,{3},NativeServiceKind::dmacman},dma_ram).status==NativeServiceStatus::unavailable &&
                  dma.invoke({33,{13,1},NativeServiceKind::dmacman},dma_ram).status==NativeServiceStatus::unsupported,
                  "DMA availability status did not match the ordinal");
            {
                std::vector<std::uint8_t> sio2_ram(0x2000);
                NativeSerialServices serial;
                auto write_sio2=initial(); write_sio2.pc=0x101a0;
                write_sio2.registers[4]=0x11111111; write_sio2.registers[5]=0x22222222; write_sio2.registers[6]=0x3bc;
                const auto wrote=run_native(nt_iop_generated_run,write_sio2,sio2_ram,32,&serial);
                check(wrote.reason==NativeStop::unmapped_code && write_sio2.pc==0x5000 && write_sio2.executed==11 &&
                      write_sio2.registers[2]==0x3bc,"SIO2 word store did not round-trip through its cached alias");
                auto read_sio2=initial(); read_sio2.pc=0x101cc;
                const auto read=run_native(nt_iop_generated_run,read_sio2,sio2_ram,32,&serial);
                check(read.reason==NativeStop::unmapped_code && read_sio2.pc==0x5000 && read_sio2.executed==8 &&
                      read_sio2.registers[2]==0x11111111 && read_sio2.registers[3]==0,
                      "SIO2 block edges did not keep the stored words");
                const auto before=sio2_ram;
                auto miss=initial(); miss.pc=0x101ec; miss.registers[4]=0x01020304;
                const auto missed=nt_iop_generated_run(miss,sio2_ram,8);
                check(missed.reason==NativeStop::address_fault && missed.address==0xbf808400 && miss.pc==0x101f4 &&
                      miss.executed==2 && sio2_ram==before,"Unmapped SIO2 store retired or wrote RAM");
                auto byte=initial(); byte.pc=0x10200; byte.registers[4]=0xff;
                const auto narrow=nt_iop_generated_run(byte,sio2_ram,8);
                check(narrow.reason==NativeStop::address_fault && narrow.address==0xbf808268 && byte.pc==0x10208 &&
                      byte.executed==2 && sio2_ram==before,"Byte SIO2 access retired");
                auto still=initial(); still.pc=0x101cc;
                const auto kept=run_native(nt_iop_generated_run,still,sio2_ram,32,&serial);
                check(kept.reason==NativeStop::unmapped_code && still.registers[2]==0x11111111 && still.registers[3]==0,
                      "Rejected SIO2 access changed a stored word");
                // Separate sessions and explicit reset cannot retain another transport's settings.
                NativeSerialServices fresh;
                auto clean=initial(); clean.pc=0x101cc;
                run_native(nt_iop_generated_run,clean,sio2_ram,32,&fresh);
                check(clean.registers[2]==0 && clean.registers[3]==0,"Serial configuration leaked between sessions");
                auto start=initial(); start.pc=0x101b0; start.registers[3]=0xbf808200; start.registers[6]=0x3bd;
                const auto rejected=run_native(nt_iop_generated_run,start,sio2_ram,1,&serial);
                check(rejected.reason==NativeStop::serial_request && start.pc==0x101b0 && start.executed==0,
                      "Unsupported serial START retired successfully");
                check(serial.invoke({0,{0x1f808268},NativeServiceKind::serial},sio2_ram).value==0x3bc,
                      "Rejected serial START changed control state");
                for (const auto address : {0x1f808260U,0x1f808264U,0x1f80826cU,0x1f808270U,0x1f808274U})
                    for (unsigned write=0;write<2;++write)
                        check(serial.invoke({write,{address,1},NativeServiceKind::serial},sio2_ram).status==NativeServiceStatus::unavailable,
                              "Unsupported serial FIFO/status operation succeeded");
                auto delayed=initial(); delayed.pc=0x101b0; delayed.registers[3]=0xbf808200;
                delayed.registers[6]=0x3bc; delayed.load_register=6; delayed.load_value=1;
                delayed.delay_slot=true; delayed.branch_target=0x5000;
                run_native(nt_iop_generated_run,delayed,sio2_ram,1,&serial);
                check(delayed.pc==0x5000 && !delayed.delay_slot && delayed.registers[6]==1 && delayed.executed==1,
                      "Serial store lost a delayed load or branch target");
                auto load=initial(); load.pc=0x101bc; load.registers[2]=0x9f808268;
                run_native(nt_iop_generated_run,load,sio2_ram,1,&serial);
                check(load.pc==0x101c0 && load.load_register==2 && load.load_value==0x3bc && load.registers[2]==0x9f808268,
                      "Serial read did not preserve the load delay");
                serial.reset();
                serial.invoke({1,{0x1f808200,2U<<8},NativeServiceKind::serial},sio2_ram);
                auto transmit=initial(); transmit.pc=0x10220; transmit.registers[4]=0x1234ab;
                const auto sent=run_native(nt_iop_generated_run,transmit,sio2_ram,8,&serial);
                check(sent.reason==NativeStop::unmapped_code && serial.pending_output().size()==1 &&
                      serial.pending_output()[0]==0xab,"Compiled serial byte write did not preserve byte width");
                transmit=initial(); transmit.pc=0x10220; transmit.registers[4]=0xcd;
                run_native(nt_iop_generated_run,transmit,sio2_ram,8,&serial);
                transmit=initial(); transmit.pc=0x10220;
                const auto excess=run_native(nt_iop_generated_run,transmit,sio2_ram,8,&serial);
                check(excess.reason==NativeStop::serial_request && transmit.pc==0x10228 &&
                      serial.pending_output().size()==2,"Serial byte write exceeded declared transaction");
                serial.reset();
                check(serial.pending_output().empty(),"Serial reset retained pending output");
                NativeInterruptServices serial_events(0x8000);
                serial.connect_disconnected_ports(serial_events);
                serial.invoke({1,{0x1f808200,(2U<<8)|(2U<<18)},NativeServiceKind::serial},sio2_ram);
                const auto short_start=serial.invoke({1,{0x1f808268,1},NativeServiceKind::serial},sio2_ram);
                check(short_start.status==NativeServiceStatus::unavailable &&
                      serial.invoke({0,{0x1f808280},NativeServiceKind::serial},sio2_ram).value==0,
                      "Incomplete serial transaction posted completion");
                for (unsigned byte_value : {0x21U,0x12U}) {
                    transmit=initial(); transmit.pc=0x10220; transmit.registers[4]=byte_value;
                    run_native(nt_iop_generated_run,transmit,sio2_ram,8,&serial);
                }
                check(serial.invoke({1,{0x1f808268,1},NativeServiceKind::serial},sio2_ram).status==NativeServiceStatus::returned &&
                      serial.invoke({0,{0x1f808268},NativeServiceKind::serial},sio2_ram).value==0 &&
                      serial.invoke({0,{0x1f808280},NativeServiceKind::serial},sio2_ram).value==1 &&
                      serial.invoke({0,{0x1f80826c},NativeServiceKind::serial},sio2_ram).value==0x1d100,
                      "Disconnected serial completion did not publish missing-device state");
                for (unsigned i=0;i<2;++i)
                    check(serial.invoke({0,{0x1f808264,0,1},NativeServiceKind::serial},sio2_ram).value==255,
                          "Disconnected serial response acknowledged a device");
                check(serial.invoke({0,{0x1f808264,0,1},NativeServiceKind::serial},sio2_ram).status==NativeServiceStatus::unavailable,
                      "Serial response underflow was hidden");
                serial.invoke({1,{0x1f808280,1},NativeServiceKind::serial},sio2_ram);
                check(serial.invoke({0,{0x1f808280},NativeServiceKind::serial},sio2_ram).value==0,
                      "Serial completion acknowledgement was lost");
                serial.reset();
                check(serial.invoke({0,{0x1f808200},NativeServiceKind::serial},sio2_ram).value==0 &&
                      serial.invoke({0,{0x1f808268},NativeServiceKind::serial},sio2_ram).value==0,
                      "Serial reset retained configuration");
                serial.invoke({1,{0x1f808200,(2U<<8)|(3U<<18)},NativeServiceKind::serial},sio2_ram);
                const auto configure_buffer=[&](unsigned channel,unsigned address,unsigned words) {
                    store_native_word(sio2_ram,0x110,channel==11 ? 1U : 0U);
                    NativeServiceRequest r{28,{channel,address,words,1},NativeServiceKind::dmacman};
                    r.stack_pointer=0x100; return serial.invoke(r,sio2_ram);
                };
                sio2_ram[0x124]=0x21; sio2_ram[0x125]=0x12;
                std::fill_n(sio2_ram.begin()+0x204,8,0x55);
                check(configure_buffer(11,0x124,2).value==1 && configure_buffer(12,0x204,2).value==1,
                      "Native serial buffers required block-size alignment instead of word alignment");
                check(serial.invoke({32,{11},NativeServiceKind::dmacman},sio2_ram).status==NativeServiceStatus::returned &&
                      serial.invoke({32,{12},NativeServiceKind::dmacman},sio2_ram).status==NativeServiceStatus::returned &&
                      sio2_ram[0x204]==0x55,"Arming serial receive fabricated a completed response");
                check(serial.invoke({1,{0x1f808268,1},NativeServiceKind::serial},sio2_ram).status==NativeServiceStatus::returned,
                      "Buffered disconnected transaction did not complete");
                for (unsigned i=0;i<8;++i) check(sio2_ram[0x204+i]==(i<3 ? 255 : 0),
                    "Serial response or block padding was not published");
                serial.reset();
                check(serial.invoke({32,{11},NativeServiceKind::dmacman},sio2_ram).status==NativeServiceStatus::unavailable &&
                      configure_buffer(12,0x1ffc,2).status==NativeServiceStatus::address_fault,
                      "Serial buffer reset or range validation failed");
            }
            NativeResult stop{};
            auto pointers=initial(); pointers.pc=0x10020; store_native_word(dma_ram,0x10180,0);
            do { stop=nt_iop_generated_run(pointers,dma_ram,1); } while (stop.reason==NativeStop::budget);
            check(stop.reason==NativeStop::unmapped_code && pointers.pc==0x5000 && pointers.executed==10 && pointers.registers[2]==1,
                  "Null loaded pointer fell through a jump delay slot into jalr");
            store_native_word(dma_ram,0x10180,0x10000);
            pointers=initial(); pointers.pc=0x10020;
            do { stop=nt_iop_generated_run(pointers,dma_ram,1); } while (stop.reason==NativeStop::budget);
            check(stop.reason==NativeStop::unmapped_code && pointers.pc==0x5000 && pointers.executed==13 && pointers.registers[2]==0x21,
                  "Constant-address loaded pointer did not reach its compiled callee");
            // A reached unknown store in another module of this bundle can name the
            // slot, so the call is open: any compiled instruction is allowed.
            auto open_pointer=initial(); open_pointer.pc=0x10040; open_pointer.registers[2]=0x1000;
            stop=nt_iop_generated_run(open_pointer,dma_ram,1);
            check(stop.reason==NativeStop::budget && open_pointer.executed==1 && open_pointer.delay_slot &&
                  open_pointer.branch_target==0x1000 && open_pointer.registers[31]==0x10048,
                  "Open loaded pointer rejected a compiled instruction");
            open_pointer=initial(); open_pointer.pc=0x10040; open_pointer.registers[2]=0x5000;
            stop=nt_iop_generated_run(open_pointer,dma_ram,1);
            check(stop.reason==NativeStop::unmapped_code && !open_pointer.executed && !open_pointer.delay_slot &&
                  open_pointer.pc==0x10040 && stop.address==0x5000 && open_pointer.registers[31]==0x5000,
                  "Open loaded pointer accepted an uncompiled address");
            for (const auto leaf : {0x10100U,0x80010100U,0xa0010100U}) {
                auto open=initial(); open.pc=0x100d0; open.registers[4]=leaf;
                do { stop=nt_iop_generated_run(open,dma_ram,1); } while (stop.reason==NativeStop::budget);
                check(stop.reason==NativeStop::unmapped_code && open.pc==0x5000 && open.executed==12 &&
                      open.registers[2]==0x33 && open.registers[3]==1,
                      "Open indirect did not call a compiled leaf and return");
            }
            for (const auto target : {0U,0x5000U,0x10101U}) {
                auto missed=initial(); missed.pc=0x100e4; missed.registers[2]=target;
                stop=nt_iop_generated_run(missed,dma_ram,1);
                check(stop.reason==NativeStop::unmapped_code && !missed.executed && !missed.delay_slot &&
                      missed.pc==0x100e4 && missed.registers[3]==0 && stop.address==target,
                      "Open indirect ran its delay slot or accepted an uncompiled address");
            }
            pointers=initial(); pointers.pc=0x10060;
            do { stop=nt_iop_generated_run(pointers,dma_ram,1); } while (stop.reason==NativeStop::budget);
            check(stop.reason==NativeStop::unmapped_code && pointers.executed==10 && pointers.registers[2]==1 &&
                  load_native_word(dma_ram,0x10184)==0,
                  "Unset stored pointer was treated as an executable target");
            pointers=initial(); pointers.pc=0x100b0;
            do { stop=nt_iop_generated_run(pointers,dma_ram,1); } while (stop.reason==NativeStop::budget);
            check(stop.reason==NativeStop::unmapped_code && pointers.executed==6 && load_native_word(dma_ram,0x10184)==0x10008,
                  "Constant pointer store did not reach its slot");
            pointers=initial(); pointers.pc=0x10060;
            do { stop=nt_iop_generated_run(pointers,dma_ram,1); } while (stop.reason==NativeStop::budget);
            check(stop.reason==NativeStop::unmapped_code && pointers.pc==0x10080 && pointers.executed==6 &&
                  pointers.registers[2]==99 && stop.address==0x10008 && load_native_word(dma_ram,0x10184)==0x10008,
                  "Retracted stored leaf was called or the slot was not written");
        }
#ifdef ANYPS2_TEST_LARGE_LLVM
        for (const auto alias : {0U,0x80000000U,0xa0000000U}) for (const auto budget : {1U,255U,256U,257U,2048U}) {
            auto large=initial(alias); large.pc=alias|0x8000; large.registers[4]=alias|0x6000;
            store_native_word(ram,0x6000,0x1234);
            NativeResult stop{};
            do { stop=nt_iop_generated_run(large,ram,budget); } while (stop.reason==NativeStop::budget);
            check(stop.reason==NativeStop::unmapped_code && large.pc==0x5000 && large.executed==1102 &&
                  large.registers[8]==1097 && large.registers[2]==0x1234 && large.registers[3]==99,
                  "Large LLVM module lost cross-function branch/load state or execution budget");
        }
#endif
        for (const auto budget : {1U,2U,20U}) {
            auto service=initial(); service.pc=0x1180; service.registers[2]=123;
            for (unsigned arg=0;arg<4;++arg) service.registers[4+arg]=arg+1;
            service.load_register=12; service.load_value=55;
            FixtureServices provider; NativeResult stop{};
            do { stop=run_native(nt_iop_generated_run,service,ram,budget,&provider); } while (stop.reason==NativeStop::budget);
            check(stop.reason==NativeStop::unmapped_code && service.executed==4 && service.registers[2]==11 &&
                  service.registers[12]==55 && !service.load_register && provider.calls==1,
                  "Native service result, pending load, continuation or budget differs");
        }
        for (const auto selector : {123U,124U,999U}) {
            auto service=initial(); service.pc=0x1180; service.registers[2]=selector;
            FixtureServices provider;
            auto stop=run_native(nt_iop_generated_run,service,ram,0,&provider);
            check(stop.reason==NativeStop::budget && !provider.calls,"Zero-budget service had side effects");
            stop=run_native(nt_iop_generated_run,service,ram,1);
            check(stop.reason==NativeStop::service_request && !service.executed && service.pc==0x1180,
                  "Missing native provider silently completed a service");
            service.delay_slot=true;
            stop=run_native(nt_iop_generated_run,service,ram,1,&provider);
            check(stop.reason==NativeStop::service_request && !provider.calls,"Delay-slot service invoked a provider");
            service.delay_slot=false;
            if (selector==123) continue;
            stop=run_native(nt_iop_generated_run,service,ram,1,&provider);
            check(stop.reason==(selector==124 ? NativeStop::address_fault : NativeStop::service_request) &&
                  !service.executed && service.pc==0x1180 && service.registers[2]==selector && provider.calls==1,
                  "Failed native service changed the compiled continuation");
        }
        for (const auto alias : {0U,0x80000000U,0xa0000000U}) for (const auto budget : {1U,8U}) {
            for (const auto argument : {0U,1U}) {
                auto trapped=initial(alias); trapped.pc=alias|0x1160; trapped.registers[4]=argument;
                NativeResult stop{};
                do { stop=nt_iop_generated_run(trapped,ram,budget); } while (stop.reason==NativeStop::budget);
                if (argument) check(stop.reason==NativeStop::unmapped_code && trapped.executed==4 && trapped.registers[2]==1,
                                    "Native conditional path did not skip the trap");
                else check(stop.reason==NativeStop::guest_break && stop.trap_code==9 && !stop.address &&
                           trapped.pc==(alias|0x1168) && trapped.executed==2 && !trapped.delay_slot,
                           "Native BREAK did not preserve its diagnostic boundary");
            }
            auto trapped=initial(alias); trapped.pc=alias|0x1174;
            auto stop=nt_iop_generated_run(trapped,ram,1);
            check(stop.reason==NativeStop::budget && trapped.delay_slot && trapped.executed==1,
                  "Could not suspend immediately before a delay-slot trap");
            trapped.load_register=12; trapped.load_value=123; trapped.registers[12]=456;
            stop=nt_iop_generated_run(trapped,ram,budget);
            check(stop.reason==NativeStop::guest_break && stop.trap_code==23 && trapped.pc==(alias|0x1178) &&
                  trapped.delay_slot && trapped.branch_target==(alias|0x117c) && trapped.executed==1 &&
                  trapped.load_register==12 && trapped.load_value==123 && trapped.registers[12]==456,
                  "Delay-slot BREAK retired state or lost its pending control transfer");
        }
        struct DivisionCase { std::uint32_t a,b,signed_q,signed_r,unsigned_q,unsigned_r; };
        constexpr DivisionCase divisions[]{
            {0,0,0xffffffff,0,0xffffffff,0},
            {1,0,0xffffffff,1,0xffffffff,1},
            {0xffffffff,0,1,0xffffffff,0xffffffff,0xffffffff},
            {0x80000000,0xffffffff,0x80000000,0,0,0x80000000},
            {0x80000000,3,0xd5555556,0xfffffffe,0x2aaaaaaa,2},
            {0xfffffff9,3,0xfffffffe,0xffffffff,0x55555553,0},
            {7,0xfffffffd,0xfffffffe,1,0,7},
            {0xfffffff9,0xfffffffd,2,0xffffffff,0,0xfffffff9},
            {0xffffffff,2,0,0xffffffff,0x7fffffff,1}
        };
        for (const auto& item : divisions) for (const auto alias : {0U,0x80000000U,0xa0000000U})
            for (const auto budget : {1U,3U,64U}) {
                auto divided=initial(alias); divided.pc=alias|0x1140;
                divided.registers[4]=item.a; divided.registers[5]=item.b;
                NativeResult stop{};
                do { stop=nt_iop_generated_run(divided,ram,budget); } while (stop.reason==NativeStop::budget);
                check(stop.reason==NativeStop::unmapped_code && divided.executed==8 &&
                      divided.registers[2]==item.signed_q && divided.registers[3]==item.signed_r &&
                      divided.registers[6]==item.unsigned_q && divided.registers[7]==item.unsigned_r &&
                      divided.lo==item.unsigned_q && divided.hi==item.unsigned_r,
                      "Compiled division zero/overflow/remainder or suspension semantics differ");
            }
        constexpr std::uint32_t signed_values[]{0,1,0x7fffffff,0x80000000,0xffffffff};
        for (const auto alias : {0U,0x80000000U,0xa0000000U}) for (const auto value : signed_values) {
            const auto number=std::bit_cast<std::int32_t>(value);
            for (const auto amount : {0U,1U,31U,32U,63U,0xffffffffU}) {
                auto shifted=initial(alias); shifted.pc=alias|0x10f0;
                shifted.registers[4]=value; shifted.registers[5]=amount;
                NativeResult stop{};
                do { stop=nt_iop_generated_run(shifted,ram,1); } while (stop.reason==NativeStop::budget);
                const auto count=amount&31;
                const auto logical=value>>count;
                const auto arithmetic=logical|((count && (value&0x80000000U)) ? (0xffffffffU<<(32-count)) : 0U);
                check(stop.reason==NativeStop::unmapped_code && shifted.executed==6 &&
                      shifted.registers[2]==((value&0x80000000U) ? 0xffffffffU : 0U) &&
                      shifted.registers[3]==(value<<count) && shifted.registers[6]==logical &&
                      shifted.registers[7]==arithmetic,"Compiled shift amount masking or sign extension differs");
            }
            const bool predicates[]{number<0,number>=0,number<=0,number>0};
            for (unsigned predicate=0;predicate<4;++predicate) for (const auto budget : {1U,7U}) {
                auto branch=initial(alias); branch.pc=alias|(0x1090+predicate*20); branch.registers[4]=value;
                NativeResult stop{};
                do { stop=nt_iop_generated_run(branch,ram,budget); } while (stop.reason==NativeStop::budget);
                check(stop.reason==NativeStop::unmapped_code && branch.registers[2]==std::uint32_t(predicates[predicate]) &&
                      branch.executed==(predicates[predicate] ? 4U : 5U),
                      "Signed branch condition, delay slot or fallthrough differs");
            }
            for (const auto other : signed_values) {
                for (const auto budget : {1U,3U,64U}) {
                    auto product=initial(alias); product.pc=alias|0x1108;
                    product.hi=0x12345678; product.lo=0xabcdef01;
                    product.registers[4]=value; product.registers[5]=other;
                    NativeResult stop{};
                    do { stop=nt_iop_generated_run(product,ram,budget); } while (stop.reason==NativeStop::budget);
                    const auto signed_product=static_cast<std::uint64_t>(std::int64_t(number)*std::bit_cast<std::int32_t>(other));
                    const auto unsigned_product=std::uint64_t(value)*other;
                    check(stop.reason==NativeStop::unmapped_code && product.executed==14 &&
                          product.registers[2]==std::uint32_t(signed_product>>32) && product.registers[3]==std::uint32_t(signed_product) &&
                          product.registers[6]==std::uint32_t(unsigned_product>>32) && product.registers[7]==std::uint32_t(unsigned_product) &&
                          product.registers[8]==value && product.registers[9]==other &&
                          product.registers[10]==0x12345678 && product.registers[11]==0xabcdef01 &&
                          product.hi==value && product.lo==other,"Compiled HI/LO transfers, multiplication or suspension differs");
                }
                auto compare=initial(alias); compare.pc=alias|0x10e0;
                compare.registers[4]=value; compare.registers[5]=other;
                const auto stop=nt_iop_generated_run(compare,ram,10);
                check(stop.reason==NativeStop::unmapped_code && compare.executed==4 &&
                      compare.registers[2]==std::uint32_t(number<std::bit_cast<std::int32_t>(other)) &&
                      compare.registers[3]==std::uint32_t(number<-1),"Signed register/immediate comparison differs");
            }
        }
        // Exhausting the slice must win even at an unmapped or faulting PC.
        // Resumption must retain pending loads and delay-slot state exactly.
        for (const auto pc : {0x1000U,0x1014U,0x5000U}) {
            auto suspended=initial(); suspended.pc=pc; suspended.delay_slot=true;
            suspended.branch_target=0x1020; suspended.load_register=12; suspended.load_value=123;
            const auto stopped=nt_iop_generated_run(suspended,ram,0);
            check(stopped.reason==NativeStop::budget && suspended.pc==pc && suspended.delay_slot &&
                  suspended.branch_target==0x1020 && suspended.load_register==12 &&
                  suspended.load_value==123 && !suspended.executed,
                  "Zero-budget execution changed suspended control or load state");
        }
        for (const auto alias : {0U,0x80000000U,0xa0000000U}) {
            auto call=initial(alias); call.pc=alias|0x1050;
            NativeResult stopped{};
            do { stopped=nt_iop_generated_run(call,ram,1); } while (stopped.reason==NativeStop::budget);
            check(stopped.reason==NativeStop::unmapped_code && call.pc==0x5000 && call.executed==11 &&
                  call.registers[2]==157 && call.registers[3]==0 && call.registers[31]==0x5000,
                  "Compiled direct call, return, bit operations or unsigned immediate compare differ");
        }
        for (const auto alias : {0U,0x80000000U,0xa0000000U}) {
            auto narrow=initial(alias); narrow.pc=alias|0x1030;
            ram[0x2000]=0x80; ram[0x2002]=0x01; ram[0x2003]=0x80;
            NativeResult stopped{};
            do { stopped=nt_iop_generated_run(narrow,ram,1); } while (stopped.reason==NativeStop::budget);
            check(stopped.reason==NativeStop::unmapped_code && narrow.executed==8 &&
                  narrow.registers[2]==0xffffff80U && narrow.registers[3]==0x80 &&
                  narrow.registers[5]==0xffff8001U && narrow.registers[6]==0x8001 &&
                  ram[0x2004]==0x80 && ram[0x2006]==1 && ram[0x2007]==0x80,
                  "Compiled byte/halfword loads, stores or signed extension differ");
            narrow=initial(alias); narrow.pc=alias|0x1038; narrow.registers[4]=alias|0x2001;
            stopped=nt_iop_generated_run(narrow,ram,1);
            check(stopped.reason==NativeStop::address_fault && !narrow.executed,
                  "Unaligned halfword load retired");
            for (const auto pc : {0x1040U,0x1044U}) {
                narrow=initial(alias); narrow.pc=alias|pc;
                narrow.registers[4]=alias|0x1001;
                if (pc==0x1044) narrow.registers[4]=alias|0x1000;
                stopped=nt_iop_generated_run(narrow,ram,1);
                check(stopped.reason==NativeStop::code_write && !narrow.executed,
                      "Partial store to compiled instruction accepted");
            }
        }
        auto cpu=initial(); cpu.registers[4]=0x300000;
        auto result=nt_iop_generated_run(cpu,ram,100);
        check(result.reason==NativeStop::address_fault && cpu.pc==0x1018 && cpu.executed==14,
              "Invalid data extent did not stop before retirement");
        cpu=initial(); cpu.pc=0x1018; cpu.registers[4]=0x300000;
        cpu.load_register=12; cpu.load_value=123; cpu.registers[12]=456;
        result=nt_iop_generated_run(cpu,ram,1);
        check(result.reason==NativeStop::address_fault && cpu.pc==0x1018 && !cpu.executed &&
              cpu.load_register==12 && cpu.load_value==123 && cpu.registers[12]==456,
              "Fault retired a pending load before the instruction completed");
        cpu=initial(); cpu.registers[4]=0x1000;
        result=nt_iop_generated_run(cpu,ram,100);
        check(result.reason==NativeStop::code_write && cpu.pc==0x1024 && load_native_word(ram,0x1004)==0,
              "Generated program silently accepted a write to converted code");
        cpu=initial(); cpu.pc=0x1010; cpu.delay_slot=true;
        result=nt_iop_generated_run(cpu,ram,1);
        check(result.reason==NativeStop::branch_in_delay && !cpu.executed,
              "Branch-in-delay changed generated execution state");
        {
            NativePrntServices prnt;
            const auto setup=[&](const std::string& format) {
                std::copy(format.begin(),format.end(),ram.begin()+0xc400); ram[0xc400+format.size()]=0;
                store_native_word(ram,0xc000,0xc100);
                auto cpu=initial(); cpu.pc=0x2b90; cpu.registers[5]=0xc000; cpu.registers[6]=0xc400;
                cpu.registers[7]=0x8000c500; cpu.registers[28]=0x1234;
                return cpu;
            };
            auto print=setup("v=%d %%"); store_native_word(ram,0xc500,static_cast<std::uint32_t>(-7));
            print.load_register=2; print.load_value=77;
            auto stopped=run_native(nt_iop_generated_run,print,ram,5,&prnt);
            check(stopped.reason==NativeStop::budget && print.native_call_owner==&prnt,
                  "prnt did not enter a budgeted compiled callback");
            auto reentrant=setup("v=%d %%");
            reentrant.pc=0x2adc; reentrant.registers[4]=0x2dd0;
            check(run_native(nt_iop_generated_run,reentrant,ram,1,&prnt).reason==NativeStop::import_request &&
                  !reentrant.executed,"prnt accepted a concurrent invocation");
            for (unsigned step=0;step<200 && stopped.reason==NativeStop::budget;++step)
                stopped=run_native(nt_iop_generated_run,print,ram,1,&prnt);
            const std::string expected="v=-7 %";
            check(stopped.reason==NativeStop::unmapped_code && print.pc==0x5000 &&
                  print.registers[2]==expected.size() && print.registers[28]==0x1234 && !print.native_call_owner,
                  "prnt did not restore the caller or ignored its character count");
            for (unsigned index=0;index<expected.size();++index)
                check(load_native_word(ram,0xc100+index*4)==static_cast<unsigned char>(expected[index]),
                      "prnt duplicated or changed a callback character");
            check(load_native_word(ram,0xc100+expected.size()*4)==513 &&
                  load_native_word(ram,0xc104+expected.size()*4)==512 &&
                  load_native_word(ram,0xc000)==0xc100+(expected.size()+2)*4,
                  "prnt lost flush/reset ordering");
            print=setup(""); print.registers[7]=0xffffffff;
            check(run_native(nt_iop_generated_run,print,ram,128,&prnt).reason==NativeStop::unmapped_code &&
                  print.registers[2]==0 && load_native_word(ram,0xc100)==513 && load_native_word(ram,0xc104)==512,
                  "Empty prnt failed to flush/reset or read unused arguments");
            for (const auto format : {"%f","%d"}) {
                print=setup(format); print.pc=0x2adc; print.registers[4]=0x2dd0; print.registers[7]=0xfffffffc;
                const auto before=ram;
                check(run_native(nt_iop_generated_run,print,ram,128,&prnt).reason==
                      (format==std::string("%f") ? NativeStop::import_request : NativeStop::address_fault) &&
                      !print.executed && ram==before,"Invalid prnt emitted partial output");
            }
            print=setup("x"); print.pc=0x2adc; print.registers[4]=0xc800;
            check(run_native(nt_iop_generated_run,print,ram,1,&prnt).reason==NativeStop::unmapped_code &&
                  !print.executed,"prnt accepted an uncompiled callback");
        }
        {
            NativeSysclibServices sysclib;
            // Both public case-conversion imports lower directly to LLVM,
            // without a service provider or locale-dependent host ctype call.
            const auto unchanged=ram;
            for (const bool upper : {false,true}) for (unsigned byte=0;byte<128;++byte) {
                const auto first=upper ? unsigned('a') : unsigned('A');
                const auto expected=byte>=first && byte<first+26 ? (upper ? byte-32 : byte+32) : byte;
                for (const unsigned pending : {0U,2U,4U,31U}) {
                    auto cpu=initial(); cpu.pc=upper ? 0x1706c : 0x17074;
                    cpu.registers[4]=byte|0x12340000U; cpu.load_register=pending;
                    cpu.load_value=pending==4 ? cpu.registers[4] : pending==31 ? 0x5000U : 0xdeadbeefU;
                    if (pending==4) cpu.registers[4]=0;
                    if (pending==31) cpu.registers[31]=0;
                    check(run_native(nt_iop_generated_run,cpu,ram,2).reason==NativeStop::unmapped_code &&
                          cpu.pc==0x5000 && cpu.registers[2]==expected && cpu.executed==1 && !cpu.load_register && ram==unchanged,
                          "Compiled ASCII conversion lost byte, load-delay or return semantics");
                }
            }
            for (const auto pc : {0x1706cU,0x17074U}) {
                auto cpu=initial(); cpu.pc=pc; cpu.registers[4]=128;
                check(run_native(nt_iop_generated_run,cpu,ram,2).reason==NativeStop::import_request &&
                      cpu.pc==pc && !cpu.executed && ram==unchanged,"ASCII conversion silently accepted an undefined high byte");
                cpu=initial(); cpu.pc=pc; cpu.delay_slot=true;
                check(run_native(nt_iop_generated_run,cpu,ram,2).reason==NativeStop::branch_in_delay &&
                      !cpu.executed,"ASCII import accepted a branch delay-slot entry");
            }
            const std::string punctuation="!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";
            for (unsigned byte=0;byte<128;++byte) {
                unsigned expected=0;
                if (byte<32 || byte==127) expected=32;
                if (byte>=9 && byte<=13) expected=8;
                if (byte==32) expected=24;
                if (punctuation.find(static_cast<char>(byte))!=std::string::npos) expected=16;
                if (std::string("0123456789").find(static_cast<char>(byte))!=std::string::npos) expected=4;
                if (std::string("ABCDEFGHIJKLMNOPQRSTUVWXYZ").find(static_cast<char>(byte))!=std::string::npos) expected=1;
                if (std::string("abcdefghijklmnopqrstuvwxyz").find(static_cast<char>(byte))!=std::string::npos) expected=2;
                if (std::string("ABCDEFabcdef").find(static_cast<char>(byte))!=std::string::npos) expected|=64;
                auto cpu=initial(); cpu.pc=0x2ad4; cpu.registers[4]=byte|0x12340000;
                cpu.load_register=2; cpu.load_value=0xdeadbeef;
                const auto before=ram;
                check(run_native(nt_iop_generated_run,cpu,ram,2,&sysclib).reason==NativeStop::unmapped_code &&
                      cpu.pc==0x5000 && cpu.registers[2]==expected && !cpu.load_register && ram==before,
                      "Native sysclib byte classification or return ABI differs");
            }
            const auto string_call=[&](unsigned pc,unsigned a,unsigned b=0,unsigned count=0) {
                auto state=initial(); state.pc=pc; state.registers[4]=a; state.registers[5]=b; state.registers[6]=count;
                state.load_register=2; state.load_value=99;
                const auto before=ram;
                check(run_native(nt_iop_generated_run,state,ram,2,&sysclib).reason==NativeStop::unmapped_code &&
                      state.pc==0x5000 && !state.load_register && ram==before,"Native string import lost caller state or wrote memory");
                return state.registers[2];
            };
            // One compiled import family covers binary search/comparison, alias
            // preservation, zero lengths and malformed spans without writes.
            const std::array<std::uint8_t,7> binary{0,0xff,0x80,'A',0,'B',0xfe};
            std::copy(binary.begin(),binary.end(),ram.begin()+0xc800);
            std::copy(binary.begin(),binary.end(),ram.begin()+0xc900);
            check(string_call(0x17044,0x8000c800,0x180,7)==0x8000c802 &&
                  string_call(0x17044,0xa000c800,0,7)==0xa000c800 &&
                  string_call(0x17044,0xc800,'B',5)==0 &&
                  string_call(0x17044,0xffffffff,'B',0)==0,"Binary search lost bounds, byte or alias semantics");
            for (const auto pc:{0x1704cU,0x17054U}) {
                check(string_call(pc,0xc800,0xa000c900,7)==0 && string_call(pc,0xffffffff,0xffffffff,0)==0,
                    "Binary comparison failed equality or zero length");
                ram[0xc902]=1;
                check(string_call(pc,0xc800,0xc900,7)==127 && string_call(pc,0xc900,0xc800,7)==0xffffff81U,
                    "Binary comparison changed the first unsigned-byte difference");
                check(string_call(pc,0xc800,0xc900,2)==0,"Binary comparison exceeded its length");
                ram[0xc902]=0x80;
            }
            for (const auto pc:{0x17044U,0x1704cU,0x17054U}) {
                auto bad=initial(); bad.pc=pc; bad.registers[4]=0xffff; bad.registers[5]=0xc800; bad.registers[6]=2;
                const auto before=ram;
                const auto failed=run_native(nt_iop_generated_run,bad,ram,1,&sysclib);
                check(failed.reason==NativeStop::address_fault && !bad.executed && ram==before,
                    "Binary import accepted an out-of-range span or mutated on failure");
            }
            const std::array<std::uint8_t,5> text{'a',0x80,'z',0,0xff};
            std::copy(text.begin(),text.end(),ram.begin()+0xc800);
            std::copy(text.begin(),text.end(),ram.begin()+0xc900);
            check(string_call(0x2cb4,0x8000c800)==3 && string_call(0x2cb4,0xc803)==0 &&
                  string_call(0x2ae4,0xc800,0xa000c900)==0,"String length/equality or address alias differs");
            ram[0xc901]=0x7f;
            check(string_call(0x2ae4,0xc800,0xc900)==1 && string_call(0x2ae4,0xc900,0xc800)==0xffffffff &&
                  string_call(0x2cbc,0xc800,0xc900,1)==0 && string_call(0x2cbc,0xc800,0xc900,2)==1 &&
                  string_call(0x2cbc,0xffffffff,0xffffffff,0)==0,"String comparison lost unsigned bytes or count bounds");
            ram.back()=0;
            check(string_call(0x2cb4,0xffff)==0 && string_call(0x2ae4,0xffff,0xc803)==0,
                  "String import read past a terminal byte at the RAM boundary");
            ram.back()='a'; ram[0xc900]='b';
            check(string_call(0x2ae4,0xffff,0xc900)==0xffffffff && string_call(0x2cbc,0xffff,0xc800,1)==0,
                  "String import read beyond a decisive comparison/count");
            for (const auto pc : {0x2cb4U,0x2ae4U,0x2cbcU}) {
                auto bad=initial(); bad.pc=pc; bad.registers[4]=0xffff; bad.registers[5]=0xc800; bad.registers[6]=2;
                const auto before=ram;
                check(run_native(nt_iop_generated_run,bad,ram,1,&sysclib).reason==NativeStop::address_fault &&
                      !bad.executed && ram==before,"Unterminated boundary string did not fault without mutation");
            }
            ram[0xc800]='a'; ram[0xc801]=0x80; ram[0xc802]='a'; ram[0xc803]=0;
            for (const auto pc : {0x2cc4U,0x2cccU,0x2cd4U,0x2cdcU}) {
                const bool last=pc==0x2cd4 || pc==0x2cdc;
                check(string_call(pc,0x8000c800,'a')==(last ? 0x8000c802U : 0x8000c800U) &&
                      string_call(pc,0xa000c800,0x180)==0xa000c801 &&
                      string_call(pc,0xc800,0)==0xc803 && string_call(pc,0xc800,'!')==0 &&
                      string_call(pc,0xc803,'a')==0,"String search lost first/last, byte truncation, NUL or alias semantics");
                ram.back()=0;
                check(string_call(pc,0xffff,0)==0xffff,"String search read beyond the terminal boundary byte");
                ram.back()='a';
                if (!last) check(string_call(pc,0xffff,'a')==0xffff,"First-match search scanned beyond its match");
                auto bad=initial(); bad.pc=pc; bad.registers[4]=0xffff; bad.registers[5]=last ? 'a' : 'z';
                const auto before=ram;
                check(run_native(nt_iop_generated_run,bad,ram,1,&sysclib).reason==NativeStop::address_fault &&
                      !bad.executed && ram==before,"Unterminated search returned a partial match or changed state");
            }
            const auto copy_call=[&](unsigned pc,unsigned destination,unsigned source,unsigned count,NativeStop expected) {
                auto state=initial(); state.pc=pc; state.registers[4]=destination; state.registers[5]=source; state.registers[6]=count;
                state.load_register=2; state.load_value=123;
                const auto before=ram;
                const auto stop=run_native(nt_iop_generated_run,state,ram,2,&sysclib);
                check(stop.reason==expected,"String copy returned an unexpected stop");
                if (expected==NativeStop::unmapped_code)
                    check(state.pc==0x5000 && state.registers[2]==destination && !state.load_register,
                          "String copy lost destination alias or pending-load return semantics");
                else check(!state.executed && state.load_register==2 && ram==before,"Rejected string copy partially wrote memory");
            };
            std::fill_n(ram.begin()+0xca00,12,0xa5);
            copy_call(0x2ce4,0xa000ca01,0x8000c800,0,NativeStop::unmapped_code);
            check(std::equal(ram.begin()+0xc800,ram.begin()+0xc804,ram.begin()+0xca01) && ram[0xca00]==0xa5 && ram[0xca05]==0xa5,
                  "strcpy lost its terminator or crossed its destination extent");
            copy_call(0x2cec,0xca01,0xc800,7,NativeStop::unmapped_code);
            check(ram[0xca04]==0 && ram[0xca05]==0 && ram[0xca06]==0 && ram[0xca07]==0 && ram[0xca08]==0xa5,
                  "strncpy did not pad exactly its remaining destination");
            ram[0xca03]=0x55;
            copy_call(0x2cec,0xca01,0xc800,2,NativeStop::unmapped_code);
            check(ram[0xca01]=='a' && ram[0xca02]==0x80 && ram[0xca03]==0x55,"Truncated strncpy added a terminator");
            copy_call(0x2cec,0xffffffff,0xffffffff,0,NativeStop::unmapped_code);
            ram.back()=0;
            copy_call(0x2cec,0xca00,0xffff,4,NativeStop::unmapped_code);
            check(load_native_word(ram,0xca00)==0,"strncpy read beyond an early terminator instead of padding");
            ram.back()='x';
            copy_call(0x2cec,0xca00,0xffff,1,NativeStop::unmapped_code);
            for (const auto pc : {0x2ce4U,0x2cecU}) {
                copy_call(pc,0xca00,0xffff,2,NativeStop::address_fault);
                copy_call(pc,0xffff,0xc800,4,NativeStop::address_fault);
                copy_call(pc,0x1000,0xc800,4,NativeStop::code_write);
                copy_call(pc,0x8000c801,0xc800,4,NativeStop::import_request);
            }
            copy_call(0x2cec,0xca00,0xc800,0x100001,NativeStop::import_request);
            const auto fill_call=[&](unsigned pc,unsigned destination,unsigned value,unsigned count,NativeStop expected) {
                auto state=initial(); state.pc=pc; state.registers[4]=destination; state.registers[5]=value; state.registers[6]=count;
                const auto before=ram;
                check(run_native(nt_iop_generated_run,state,ram,2,&sysclib).reason==expected,"Native fill returned an unexpected stop");
                if (expected==NativeStop::unmapped_code) {
                    check(state.pc==0x5000 && (pc!=0x2d6c || state.registers[2]==destination),"Word fill lost its destination return");
                } else check(!state.executed && ram==before,"Rejected native fill partially wrote memory");
            };
            std::fill_n(ram.begin()+0xca00,24,0xa5);
            fill_call(0x2d64,0xa000ca01,7,0,NativeStop::unmapped_code);
            check(std::all_of(ram.begin()+0xca01,ram.begin()+0xca08,[](auto byte){return byte==0;}) &&
                  ram[0xca00]==0xa5 && ram[0xca08]==0xa5,"bzero changed the wrong byte extent");
            fill_call(0x2d6c,0x8000ca04,0x12345678,11,NativeStop::unmapped_code);
            check(load_native_word(ram,0xca04)==0x12345678 && load_native_word(ram,0xca08)==0x12345678 &&
                  ram[0xca0c]==0xa5 && ram[0xca0e]==0xa5,"Word fill lost byte count, value or trailing-byte preservation");
            fill_call(0x2d64,0xffffffff,0,0,NativeStop::unmapped_code);
            fill_call(0x2d6c,0xffffffff,99,3,NativeStop::unmapped_code);
            fill_call(0x2d6c,0xca01,1,4,NativeStop::address_fault);
            fill_call(0x2d64,0xffff,2,0,NativeStop::address_fault);
            fill_call(0x2d6c,0xfffc,1,8,NativeStop::address_fault);
            fill_call(0x2d64,0xca00,0xffffffff,0,NativeStop::address_fault);
            fill_call(0x2d6c,0xca00,1,0xffffffff,NativeStop::address_fault);
            fill_call(0x2d64,0x1001,1,0,NativeStop::code_write);
            fill_call(0x2d6c,0x1000,1,4,NativeStop::code_write);
            const auto sprintf_call=[&](const std::string& format,unsigned destination,NativeStop expected,unsigned stack=0x8000c500) {
                std::copy(format.begin(),format.end(),ram.begin()+0xc400); ram[0xc400+format.size()]=0;
                auto state=initial(); state.pc=0x2aec; state.registers[4]=destination; state.registers[5]=0xa000c400;
                state.registers[6]=static_cast<std::uint32_t>(-7); state.registers[7]=0x2a; state.registers[29]=stack;
                state.load_register=2; state.load_value=123;
                const auto before=ram;
                check(run_native(nt_iop_generated_run,state,ram,2,&sysclib).reason==expected,"sprintf returned an unexpected stop");
                if (expected==NativeStop::unmapped_code)
                    check(state.pc==0x5000 && !state.load_register && state.registers[29]==stack,"sprintf lost its caller state");
                else check(!state.executed && state.load_register==2 && ram==before,"Rejected sprintf partially wrote output");
                return state.registers[2];
            };
            ram[0xc600]='o'; ram[0xc601]='k'; ram[0xc602]=0;
            store_native_word(ram,0xc510,0x8000c600); store_native_word(ram,0xc514,'!');
            std::fill_n(ram.begin()+0xcb00,32,0xa5);
            const std::string printed="-7 002a ok !";
            check(sprintf_call("%d %04x %s %c",0x8000cb01,NativeStop::unmapped_code)==printed.size() &&
                  std::equal(printed.begin(),printed.end(),ram.begin()+0xcb01) && ram[0xcb01+printed.size()]==0 &&
                  ram[0xcb00]==0xa5 && ram[0xcb02+printed.size()]==0xa5,"sprintf lost register/stack arguments or terminator bounds");
            check(sprintf_call("",0xffff,NativeStop::unmapped_code,0xffffffff)==0 && ram.back()==0,
                  "Empty sprintf did not write exactly a terminator or read unused arguments");
            const std::string pointers="0xfffffff9 0x0000002a 0x8000c600";
            check(sprintf_call("%p %p %p",0xa000cb01,NativeStop::unmapped_code)==pointers.size() &&
                std::equal(pointers.begin(),pointers.end(),ram.begin()+0xcb01) && ram[0xcb01+pointers.size()]==0,
                "Compiled sprintf pointer output differs from diagnostic formatting");
            sprintf_call("%p",0xffff,NativeStop::address_fault);
            sprintf_call("%p",0x1000,NativeStop::code_write);
            sprintf_call("%p %p %p",0xcb00,NativeStop::address_fault,0xfffffff8);
            sprintf_call("%d",0xffff,NativeStop::address_fault);
            sprintf_call("%d",0x1000,NativeStop::code_write);
            sprintf_call("prefix %f",0xcb00,NativeStop::import_request);
            sprintf_call("%d %x %s",0xcb00,NativeStop::address_fault,0xfffffff8);
            store_native_word(ram,0xc510,0xffffffff);
            sprintf_call("%d %x %s",0xcb00,NativeStop::address_fault);
            ram[0xcc00]='A'; ram[0xcc01]=0; ram[0xcc02]=0xa5; ram[0xcc05]=0xa5;
            ram[0xc800]='b'; ram[0xc801]='c'; ram[0xc802]=0;
            copy_call(0x2e64,0x8000cc00,0xa000c800,0,NativeStop::unmapped_code);
            check(ram[0xcc00]=='A' && ram[0xcc01]=='b' && ram[0xcc02]=='c' && ram[0xcc03]==0 && ram[0xcc05]==0xa5,
                  "strcat lost its prefix, append contents or terminator");
            copy_call(0x2e64,0xcc00,0xc802,0,NativeStop::unmapped_code);
            ram[0xcc00]=0;
            copy_call(0x2e64,0xcc00,0xc800,0,NativeStop::unmapped_code);
            check(ram[0xcc00]=='b' && ram[0xcc01]=='c' && ram[0xcc02]==0,"strcat could not append to an empty destination");
            ram.back()=0;
            copy_call(0x2e64,0xffff,0xc802,0,NativeStop::unmapped_code);
            copy_call(0x2e64,0xffff,0xc800,0,NativeStop::address_fault);
            ram.back()='a';
            copy_call(0x2e64,0xffff,0xc800,0,NativeStop::address_fault);
            copy_call(0x2e64,0xcc00,0xffff,0,NativeStop::address_fault);
            copy_call(0x2e64,0xcc00,0x8000cc00,0,NativeStop::import_request);
            copy_call(0x2e64,0xcc00,0xcc01,0,NativeStop::import_request);
            copy_call(0x2e64,0x1000,0xc800,0,NativeStop::code_write);
            const auto number_call=[&](const std::string& text,unsigned base,unsigned expected,unsigned consumed) {
                std::copy(text.begin(),text.end(),ram.begin()+0xcd00); ram[0xcd00+text.size()]=0;
                store_native_word(ram,0xce00,0xdeadbeef);
                auto state=initial(); state.pc=0x2e6c; state.registers[4]=0x8000cd00;
                state.registers[5]=0xa000ce00; state.registers[6]=base;
                state.load_register=2; state.load_value=123;
                check(run_native(nt_iop_generated_run,state,ram,2,&sysclib).reason==NativeStop::unmapped_code &&
                      state.pc==0x5000 && state.registers[2]==expected && !state.load_register &&
                      load_native_word(ram,0xce00)==0x8000cd00+consumed,"strtol value/end pointer or ABI differs");
            };
            number_call("  -42!",10,static_cast<unsigned>(-42),5);
            number_call("+17",0,17,3); number_call("0779",0,63,3);
            number_call("0xAf?",0,175,4); number_call("0B1012",0,5,5);
            number_call("zZ",36,1295,2); number_call("-2147483648",10,0x80000000,11);
            number_call("2147483647",10,0x7fffffff,10);
            number_call("2147483648999x",10,0x7fffffff,13);
            number_call("-999999999999!",10,0x80000000,13);
            number_call("+",10,0,0); number_call("0x",0,0,0); number_call("xyz",10,0,0);
            number_call("001",1,0,2); number_call("123",37,0,0);
            const auto bad_number=[&](unsigned source,unsigned end,unsigned base,NativeStop expected) {
                auto state=initial(); state.pc=0x2e6c; state.registers[4]=source; state.registers[5]=end; state.registers[6]=base;
                const auto before=ram;
                check(run_native(nt_iop_generated_run,state,ram,1,&sysclib).reason==expected && !state.executed && ram==before,
                      "Rejected strtol changed output or retired its call");
            };
            bad_number(0xcd00,0x1000,10,NativeStop::code_write);
            bad_number(0xcd00,0xce01,10,NativeStop::address_fault);
            bad_number(0xffffffff,0xce00,10,NativeStop::address_fault);
            ram.back()='1'; bad_number(0xffff,0xce00,10,NativeStop::address_fault);
            ram[0xcd00]=0x80; bad_number(0xcd00,0xce00,10,NativeStop::import_request);
            auto no_end=initial(); no_end.pc=0x2e6c; no_end.registers[4]=0xffffffff; no_end.registers[6]=37;
            check(run_native(nt_iop_generated_run,no_end,ram,2,&sysclib).reason==NativeStop::unmapped_code &&
                  no_end.registers[2]==0,"Invalid-base strtol read its unused input");
            auto cpu=initial(); cpu.pc=0x2ad4; cpu.registers[4]=0xffffffff;
            check(run_native(nt_iop_generated_run,cpu,ram,1,&sysclib).reason==NativeStop::import_request &&
                  !cpu.executed,"Undefined non-ASCII classification silently succeeded");
        }
        {
            NativeDriverServices drivers;
            store_native_word(ram,0xc000,0xc040); store_native_word(ram,0xc004,0x10);
            store_native_word(ram,0xc008,0); store_native_word(ram,0xc010,0xc020);
            store_native_word(ram,0xc020,0x2860); store_native_word(ram,0xc024,0x2880);
            const std::string name="fixture";
            std::copy(name.begin(),name.end(),ram.begin()+0xc040); ram[0xc040+name.size()]=0;
            const auto call=[&](unsigned pc,unsigned argument) {
                auto cpu=initial(); cpu.pc=pc; cpu.registers[4]=argument; cpu.registers[28]=0x1234;
                const auto stop=run_native(nt_iop_generated_run,cpu,ram,32,&drivers);
                check(stop.reason==NativeStop::unmapped_code && cpu.pc==0x5000 &&
                      cpu.registers[28]==0x1234 && !cpu.native_call_owner,
                      "Compiled driver lifecycle lost its caller continuation");
                return cpu.registers[2];
            };
            auto adding=initial(); adding.pc=0x281c; adding.registers[4]=0x8000c000;
            adding.load_register=12; adding.load_value=99;
            const auto driver_stop=run_native(nt_iop_generated_run,adding,ram,1,&drivers);
            check(driver_stop.reason==NativeStop::budget &&
                  adding.pc==0x2860 && adding.registers[4]==0x8000c000 && adding.registers[12]==99 &&
                  adding.native_call_owner==&drivers && drivers.size()==0,
                  "AddDrv published before initialization or failed to enter compiled init");
            check(call(0x281c,0xc000)==0xffffffffU,"Concurrent registration did not reject reentry");
            adding.native_call_owner=nullptr;
            check(drivers.invoke({0,{0,0,0,0},NativeServiceKind::native_return,0,false,0,0,&adding},ram).status==NativeServiceStatus::unsupported,
                  "Driver registry consumed another continuation's return");
            adding.native_call_owner=&drivers;
            check(run_native(nt_iop_generated_run,adding,ram,32,&drivers).reason==NativeStop::unmapped_code &&
                  adding.pc==0x5000 && adding.registers[2]==0 && !adding.native_call_owner && drivers.size()==1,
                  "Successful compiled initialization did not publish the native driver");
            check(drivers.invoke({4,{0,0,0,0},NativeServiceKind::ioman},ram).status==NativeServiceStatus::unsupported,
                  "Ready driver registry did not delegate file operations");
            check(call(0x2824,0xc040)==0 && drivers.size()==0,
                  "Legacy DelDrv did not execute teardown and ignore its negative result");
            check(call(0x2824,0xc040)==0xffffffffU,"Missing driver deletion succeeded");
            store_native_word(ram,0xc008,0xffffffff);
            check(call(0x281c,0xc000)==0xffffffffU && drivers.size()==0,
                  "Failed initializer left a registered device");
            store_native_word(ram,0xc008,1);
            for (unsigned index=0;index<16;++index) check(call(0x281c,0xc000)==0,"Driver table filled prematurely");
            check(call(0x281c,0xc000)==0xffffffffU && drivers.size()==16,"Driver table exceeded its legacy bound");
            for (unsigned index=0;index<16;++index) check(call(0x2824,0xc040)==0,"Duplicate-name lifecycle order lost a registration");
            store_native_word(ram,0xc024,0xc100);
            auto invalid=initial(); invalid.pc=0x281c; invalid.registers[4]=0xc000;
            const auto before=ram;
            check(run_native(nt_iop_generated_run,invalid,ram,32,&drivers).reason==NativeStop::unmapped_code &&
                  !invalid.executed && !drivers.size() && ram==before,
                  "Uncompiled teardown target changed registry or memory");
            invalid.pc=0x281c; invalid.registers[4]=0xfffc;
            check(run_native(nt_iop_generated_run,invalid,ram,32,&drivers).reason==NativeStop::address_fault &&
                  !invalid.executed && !drivers.size(),"Truncated driver descriptor was accepted");
        }
        {
            NativeFileDescriptors descriptors;
            NativeArchiveServices archives(descriptors);
            const auto image=fixture::reboot_image();
            std::copy(image.begin(),image.end(),ram.begin()+0xc400);
            const auto mount_call=[&](std::uint32_t entry,unsigned unit) {
                auto cpu=initial(); cpu.pc=entry; cpu.registers[4]=unit; cpu.registers[5]=0xc400;
                const auto result=run_native(nt_iop_generated_run,cpu,ram,1,&archives);
                return std::pair{result.reason,cpu.registers[2]};
            };
            check(mount_call(0xb4d4,1).first==NativeStop::import_request,"Uninitialized archive service accepted a compiled call");
            check(archives.initialize() && !archives.initialize(),"Native archive initialization did not retain ownership");
            const auto path=[&](std::string_view value) {
                std::copy(value.begin(),value.end(),ram.begin()+0xc000); ram[0xc000+value.size()]=0;
            };
            const auto call=[&](unsigned ordinal,std::uint32_t a,std::uint32_t b=0,std::uint32_t c=0) {
                auto cpu=initial(); cpu.pc=0x1614+(ordinal-4)*8;
                if (ordinal==8) cpu.pc=0x162c;
                cpu.registers[4]=a; cpu.registers[5]=b; cpu.registers[6]=c;
                const auto result=run_native(nt_iop_generated_run,cpu,ram,1,&archives);
                return std::pair{result.reason,cpu.registers[2]};
            };
            path("rom0:EXAMPLE");
            check(call(4,0xc000,1).second==static_cast<std::uint32_t>(-6),"Archive mount fabricated firmware files");
            check(mount_call(0xb4d4,1)==std::pair{NativeStop::budget,0U} && !archives.mount(1,image),"Compiled archive mount ownership failed");
            path("rom1:MISSING");
            check(call(4,0xc000,1).second==static_cast<std::uint32_t>(-2),"Missing archive member succeeded");
            path("rom1:EXAMPLE");
            check(call(4,0xc000,2).second==static_cast<std::uint32_t>(-13),"Archive allowed a write open");
            const auto fd=call(4,0xc000,1);
            check(fd==std::pair{NativeStop::budget,3U} && !archives.unmount(1),"Compiled archive open or live mount ownership failed");
            const auto before=ram;
            check(call(6,3,0x1000,19).first==NativeStop::code_write && ram==before &&
                  call(6,3,0xffff,19).first==NativeStop::address_fault,"Archive read failed to protect output");
            check(call(6,3,0xa000c100,100)==std::pair{NativeStop::budget,19U} &&
                  std::equal(image.begin()+96,image.end(),ram.begin()+0xc100) && call(6,3,0xffffffff,1).second==0,
                  "Compiled archive read/EOF or rejected-read cursor changed");
            check(call(8,3,0xfffffffd,2).second==16 && call(6,3,0xc200,10).second==3 &&
                  call(8,3,0xffffffff,0).second==19,"Archive seek did not retain ROM filesystem offset semantics");
            check(call(5,3).second==0 && !descriptors.find(3) && mount_call(0xb4dc,1).second==0,"Archive close leaked descriptor or mount");
            check(archives.mount(1,image),"Archive could not remount after all files closed");
            std::array<std::uint32_t,8> opened{};
            for (auto& handle:opened) {
                const auto result=call(4,0xc000,1); handle=result.second;
                check(result.first==NativeStop::budget && handle>=3 && handle<=10,"Archive handle allocation failed");
            }
            check(call(4,0xc000,1).second==static_cast<std::uint32_t>(-12) &&
                  call(6,opened[0],0xc100,1).second==1 && call(6,opened[1],0xc200,1).second==1 &&
                  ram[0xc100]==ram[0xc200],"Archive handle limit or independent cursors failed");
            for (const auto handle:opened) check(call(5,handle).second==0,"Archive descriptor close failed");
            check(archives.unmount(1),"Closed archive still busy");
            auto invalid=image; invalid[12]=1;
            bool rejected=false;
            try { archives.mount(0,std::move(invalid)); } catch (const std::exception&) { rejected=true; }
            check(rejected,"Archive mount accepted a firmware RESET entry");
        }
        {
            struct TemporaryImage {
                std::filesystem::path path=std::filesystem::temp_directory_path()/
                    ("anyps2-native-disc-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".iso");
                ~TemporaryImage() { std::error_code ignored; std::filesystem::remove(path,ignored); }
            } image;
            nt::Bytes payload(5003);
            for (unsigned index=0;index<payload.size();++index) payload[index]=static_cast<std::uint8_t>(index*37);
            auto disc_image=fixture::single_file_iso("DATA.BIN;1",payload);
            const std::array<std::uint8_t,7> recording_date{124,2,29,12,34,56,0};
            std::copy(recording_date.begin(),recording_date.end(),disc_image.begin()+20*2048+18);
            auto directory_offset=20*2048+disc_image[20*2048];
            directory_offset+=fixture::record(disc_image,directory_offset,"ZERO.BIN;1",21,0,false);
            const auto empty_sector=static_cast<std::uint32_t>(disc_image.size()/2048);
            disc_image.resize(disc_image.size()+2048);
            fixture::record(disc_image,directory_offset,"EMPTY",empty_sector,2048,true);
            fixture::both32(disc_image,16*2048+80,static_cast<std::uint32_t>(disc_image.size()/2048));
            nt::write_file(image.path,disc_image);
            {
                std::vector<std::uint8_t> data(0x20000);
                auto volume=std::make_unique<nt::disc::Iso9660>(image.path);
                NativeCdvdmanServices cdvd;
                cdvd.attach([&] { return volume.get(); },[] { return 2U; });
                const auto call=[&](unsigned ordinal,std::array<std::uint32_t,4> arguments={}) {
                    constexpr unsigned ordinals[]{6,8,11,12,13,14,28,37,39,83};
                    const auto index=std::find(std::begin(ordinals),std::end(ordinals),ordinal)-std::begin(ordinals);
                    auto cpu=initial(); cpu.pc=0x17814+static_cast<unsigned>(index)*8;
                    std::copy(arguments.begin(),arguments.end(),cpu.registers.begin()+4);
                    const auto result=run_native(nt_iop_generated_run,cpu,data,64,&cdvd);
                    return std::pair{result,cpu};
                };
                check(call(12).second.registers[2]==0x14 && call(13,{1}).second.registers[2]==2 &&
                      call(28).second.registers[2]==0x0a,"Native disc queries did not reflect the open volume");
                check(call(14,{2,0x18204}).second.registers[2]==1 && load_native_word(data,0x18204)==0 &&
                      call(14,{0,0x18204}).first.reason==NativeStop::import_request,
                      "Native mounted ISO fabricated eject support");
                auto registration=initial(); registration.pc=0x17920;
                check(run_native(nt_iop_generated_run,registration,data,64,&cdvd).reason==NativeStop::unmapped_code &&
                      registration.pc==0x5000 && !registration.registers[2],"Compiled disc callback was not discovered");
                data[0x18101]=1;
                const auto read=call(6,{21,1,0xa0019000,0x18100});
                check(read.second.registers[2]==1 && std::equal(payload.begin(),payload.begin()+2048,data.begin()+0x19000) &&
                      !load_native_word(data,0x18200) && call(11,{1}).second.registers[2]==0 && !call(8).second.registers[2],
                      "Native sector read did not publish real bytes before deferred completion");
                check(!call(6,{21,1,0x19000,0x18100}).second.registers[2],"Pending completion was overwritten");
                NativeCallbackMux callbacks({&cdvd}); NativeInterruptServices interrupts(0x1f000,&callbacks);
                NativeServiceRouter router({&interrupts,&cdvd});
                interrupts.invoke({9,{},NativeServiceKind::intrman},data);
                auto foreground=initial(); foreground.pc=0x5000;
                NativeResult completed;
                do { completed=interrupts.run(nt_iop_generated_run,foreground,data,1,router); } while (completed.reason==NativeStop::budget);
                check(completed.reason==NativeStop::unmapped_code && foreground.pc==0x5000 &&
                      load_native_word(data,0x18200)==1 && !cdvd.take(),"Compiled disc completion did not run exactly once");
                check(call(37).second.registers[2]==0x17900,"Disc callback removal lost the old pointer");
                check(call(37,{0x17b80}).first.reason==NativeStop::unmapped_code,
                      "Disc completion accepted an uncompiled callback");
                check(call(39).second.registers[2]==1 && call(8).second.registers[2]==1 && !cdvd.take(),
                      "Native read cancellation lost its error or invented an unregistered callback");
                const auto before=data;
                check(call(6,{21,1,0x17700,0x18100}).first.reason==NativeStop::code_write && data==before &&
                      call(6,{21,1,0x19002,0x18100}).first.reason==NativeStop::address_fault && data==before,
                      "Rejected sector destination changed native data");
                data[0x18102]=1;
                check(call(6,{21,1,0x19000,0x18100}).first.reason==NativeStop::import_request,"Missing raw sectors were fabricated");
                data[0x18102]=0;
                check(!call(6,{0xffffffff,1,0x19000,0x18100}).second.registers[2] && call(8).second.registers[2]==0x20,
                      "Out-of-volume read lost its error state");
                check(call(83,{0x18204,0x18208}).second.registers[2]==1 &&
                      !load_native_word(data,0x18204) && !load_native_word(data,0x18208),"Single-volume geometry was not validated");
                volume.reset();
                check(call(13,{1}).second.registers[2]==6 && !call(12).second.registers[2] &&
                      !call(6,{21,1,0x19000,0x18100}).second.registers[2] && call(8).second.registers[2]==0x12,
                      "Closed disc session reported ready or completed a read");
                auto layered=disc_image;
                const auto boundary=static_cast<std::uint32_t>(layered.size()/2048);
                layered.resize(layered.size()+32*2048);
                std::copy_n(disc_image.begin()+16*2048,2048,layered.begin()+boundary*2048);
                fixture::both32(layered,boundary*2048+80,32);
                std::fill(layered.end()-2048,layered.end(),0x6b);
                nt::write_file(image.path,layered); volume=std::make_unique<nt::disc::Iso9660>(image.path);
                check(call(83,{0x18204,0x18208}).second.registers[2]==1 && load_native_word(data,0x18204)==1 &&
                      load_native_word(data,0x18208)==boundary,"Appended volume geometry did not come from the ISO");
                check(call(6,{boundary+16,2,0x19000,0x18100}).second.registers[2]==1 &&
                      std::equal(layered.begin()+boundary*2048,layered.begin()+(boundary+2)*2048,data.begin()+0x19000),
                      "CDVDMAN appended-volume LSN did not read the actual descriptor and following sector");
                check(call(6,{21,1,0x19000,0x18100}).second.registers[2]==1 &&
                      std::equal(payload.begin(),payload.begin()+2048,data.begin()+0x19000),
                      "Appended-volume translation changed a first-volume read");
                const auto logical_end=static_cast<std::uint32_t>(layered.size()/2048)+16;
                check(call(6,{logical_end-1,1,0x19000,0x18100}).second.registers[2]==1 &&
                      std::equal(layered.end()-2048,layered.end(),data.begin()+0x19000) &&
                      !call(6,{logical_end,1,0x19000,0x18100}).second.registers[2] && call(8).second.registers[2]==0x20,
                      "Translated DVD extent did not preserve the final sector or reject an overrun");
                volume.reset(); layered[boundary*2048+1]=0; nt::write_file(image.path,layered);
                volume=std::make_unique<nt::disc::Iso9660>(image.path);
                const auto saved=data;
                check(call(83,{0x18204,0x18208}).first.reason==NativeStop::import_request && data==saved,
                      "Unknown layer geometry was fabricated or partially published");
                check(call(6,{boundary+16,1,0x19000,0x18100}).first.reason==NativeStop::import_request && data==saved,
                      "Unknown DVD layout was read using an invented sector mapping");
                volume.reset(); nt::write_file(image.path,disc_image);
            }
            NativeFileDescriptors descriptors;
            NativeEventServices disc_events;
            NativeDiscServices files(image.path,&descriptors,&disc_events);
            const auto path=[&](const std::string& name) {
                std::copy(name.begin(),name.end(),ram.begin()+0x2000); ram[0x2000+name.size()]=0;
            };
            path("cdrom0:");
            const auto disc_event_query=[&](std::uint32_t output,std::uint32_t size=4,
                                           std::uint32_t command=0x4391) {
                auto cpu=initial(); cpu.pc=0x282c; cpu.registers[4]=0x80002000;
                cpu.registers[5]=command; cpu.registers[6]=0; cpu.registers[7]=0;
                cpu.registers[29]=0x8000c460;
                store_native_word(ram,0xc470,output); store_native_word(ram,0xc474,size);
                const auto before=ram;
                const auto stop=run_native(nt_iop_generated_run,cpu,ram,1,&files);
                if (stop.reason!=NativeStop::budget)
                    check(ram==before && !cpu.executed,"Rejected disc event query changed state");
                return std::pair{stop.reason,cpu.registers[2]};
            };
            check(disc_event_query(0x1000).first==NativeStop::code_write &&
                  disc_event_query(0x10000).first==NativeStop::address_fault &&
                  disc_event_query(0x4000,3).second==static_cast<std::uint32_t>(-22) &&
                  disc_event_query(0x4000,4,0x4392).first==NativeStop::import_request,
                  "Disc event query accepted invalid output or unsupported command");
            check(disc_event_query(0xa0004000)==std::pair{NativeStop::budget,0U},
                  "Compiled disc event query failed");
            const auto disc_event=load_native_word(ram,0x4000);
            check(disc_event && disc_event_query(0x4004).second==0 && load_native_word(ram,0x4004)==disc_event,
                  "Disc event query did not preserve handle identity");
            check(disc_events.invoke({11,{disc_event,1,0,0},NativeServiceKind::thevent},ram).value==
                  static_cast<std::uint32_t>(-421),"Disc event query invented completion");
            check(disc_events.invoke({6,{disc_event,1},NativeServiceKind::thevent},ram).value==0 &&
                  disc_events.invoke({11,{disc_event,1,0x10,0x4010},NativeServiceKind::thevent},ram).value==0 &&
                  load_native_word(ram,0x4010)==1 &&
                  disc_events.invoke({11,{disc_event,1,0,0},NativeServiceKind::thevent},ram).value==
                  static_cast<std::uint32_t>(-421),"Disc handle is not backed by a consumable native event");
            const auto file_call=[&](unsigned ordinal,std::uint32_t a=0,std::uint32_t b=0,std::uint32_t c=0) {
                auto cpu=initial();
                const std::map<unsigned,std::uint32_t> entries{{4,0x1614},{5,0x161c},{6,0x1624},{8,0x162c},
                    {16,0x2394},{13,0x239c},{14,0x23a4},{15,0x23ac},{7,0x23b4},{10,0x23bc},{11,0x23c4},
                    {12,0x23cc},{17,0x23d4},{18,0x23dc},{9,0x23ec},{34,0x2814},{22,0x2f44}};
                cpu.pc=entries.at(ordinal);
                cpu.registers[4]=a; cpu.registers[5]=b; cpu.registers[6]=c;
                const auto stop=run_native(nt_iop_generated_run,cpu,ram,1,&files);
                check(stop.reason==NativeStop::budget && cpu.executed==1 && cpu.pc==0x5000,"Compiled disc call did not return");
                return cpu.registers[2];
            };
            path(R"(cdrom0:\DATA.BIN;1)");
            check(file_call(16,0x80002000,0xa0004000)==0 && load_native_word(ram,0x4000)==0x14 &&
                  load_native_word(ram,0x4008)==payload.size() && load_native_word(ram,0x4024)==0,
                  "Native disc stat lost file type, size or high-size initialization");
            const std::array<std::uint8_t,8> expected_date{0,56,34,12,29,2,0xe8,7};
            for (unsigned offset : {12U,20U,28U}) check(std::equal(expected_date.begin(),expected_date.end(),ram.begin()+0x4000+offset),
                  "Native disc stat did not preserve the ISO recording date");
            for (const auto output : {0x1000U,0xfff0U}) {
                auto stat=initial(); stat.pc=0x2394; stat.registers[4]=0x2000; stat.registers[5]=output;
                const auto before=ram;
                check(run_native(nt_iop_generated_run,stat,ram,1,&files).reason==
                      (output==0x1000 ? NativeStop::code_write : NativeStop::address_fault) && ram==before && !stat.executed,
                      "Rejected stat output partially changed RAM or retired");
            }
            path("cdrom0:/MISSING.BIN");
            const auto before_missing_stat=ram;
            check(file_call(16,0x2000,0x4000)==static_cast<std::uint32_t>(-2) && ram==before_missing_stat,
                  "Missing disc stat overwrote its output");
            path("cdrom0:/");
            check(file_call(16,0x2000,0x4000)==0 && load_native_word(ram,0x4000)==0x25 && load_native_word(ram,0x4008)==2048,
                  "Root-directory metadata was not resolved from the disc");
            path(R"(cdrom0:\DATA.BIN;1)");
            check(file_call(13,0x2000)==static_cast<std::uint32_t>(-20),"Directory open accepted a regular file");
            path("cdrom0:/");
            const auto directory=file_call(13,0x2000),other_directory=file_call(13,0x2000);
            check(directory==3 && other_directory==4 && file_call(5,directory)==static_cast<std::uint32_t>(-9) &&
                  file_call(6,directory,0x4000,0)==static_cast<std::uint32_t>(-21),
                  "Directory handles collided or allowed file-only operations");
            for (const auto output : {0x1000U,0xff00U}) {
                auto read_dir=initial(); read_dir.pc=0x23ac; read_dir.registers[4]=directory; read_dir.registers[5]=output;
                const auto before=ram;
                check(run_native(nt_iop_generated_run,read_dir,ram,1,&files).reason==
                      (output==0x1000 ? NativeStop::code_write : NativeStop::address_fault) && ram==before,
                      "Rejected directory output partially modified RAM");
            }
            check(file_call(15,directory,0x80004000)==1 && load_native_word(ram,0x4008)==payload.size() &&
                  std::string(reinterpret_cast<const char*>(ram.data()+0x4028))=="DATA.BIN;1" && load_native_word(ram,0x4128)==0 &&
                  file_call(15,other_directory,0x4200)==1 && load_native_word(ram,0x4208)==payload.size(),
                  "Directory read lost its entry, advanced on rejection or shared cursor state");
            check(file_call(15,directory,0x4000)==1 && load_native_word(ram,0x4008)==0 &&
                  std::string(reinterpret_cast<const char*>(ram.data()+0x4028))=="ZERO.BIN;1" &&
                  file_call(15,directory,0x4000)==1 && load_native_word(ram,0x4000)==0x25 &&
                  std::string(reinterpret_cast<const char*>(ram.data()+0x4028))=="EMPTY",
                  "Directory iteration lost empty files or directory metadata");
            const auto before_directory_eof=ram;
            check(file_call(15,directory,0x4000)==0 && ram==before_directory_eof &&
                  file_call(14,directory)==0 && file_call(14,other_directory)==0 &&
                  file_call(15,directory,0x4000)==static_cast<std::uint32_t>(-9),
                  "Directory EOF or close did not preserve its contract");
            path("cdrom0:/EMPTY");
            const auto empty=file_call(13,0x2000);
            check(empty==3 && file_call(15,empty,0x4000)==0 && file_call(14,empty)==0,
                  "Empty subdirectory could not be enumerated");
            path(R"(cdrom0:\DATA.BIN;1)");
            const auto fd=file_call(4,0x80002000,1);
            check(fd==3 && file_call(6,fd,0xa0003001,2051)==2051,"Native disc open or cross-sector read failed");
            check(std::equal(payload.begin(),payload.begin()+2051,ram.begin()+0x3001),"Native disc payload differs");
            {
                NativeHeapServices heap(static_cast<std::uint32_t>(ram.size()),0xd000,0xe000);
                NativeDriverServices drivers;
                NativeDriverFiles driver_files(drivers,descriptors,heap,[](std::string_view name) { return name=="cdrom"; });
                NativeServiceRouter router({&drivers,&driver_files,&files,&heap});
                {
                    const std::string absent="absent7:";
                    std::copy(absent.begin(),absent.end(),ram.begin()+0xc080); ram[0xc080+absent.size()]=0;
                    const auto before=ram;
                    for (const auto ordinal:{4U,10U,11U,12U,13U,16U,17U,18U,25U,26U,27U,28U,29U,31U,32U,33U})
                        check(router.invoke({ordinal,{0xc080,0xffffffff,0xffffffff,0xffffffff},NativeServiceKind::ioman},ram).value==static_cast<unsigned>(-19) && ram==before,
                              "Absent device accepted a path operation or touched its unused buffers");
                    check(driver_files.invoke({31,{0x2000,0xffffffff},NativeServiceKind::ioman},ram).status==NativeServiceStatus::unsupported,
                          "Native mount was incorrectly classified as an absent compiled driver");
                    check(router.invoke({31,{0xffffffff},NativeServiceKind::ioman},ram).status==NativeServiceStatus::address_fault,
                          "Absent-device resolution bypassed path validation");
                }
                store_native_word(ram,0xc000,0xc040); store_native_word(ram,0xc004,0x10);
                store_native_word(ram,0xc008,1); store_native_word(ram,0xc010,0xc300);
                for (const auto [offset,target] : {std::pair{0U,0x2860U},{4U,0x2880U},{12U,0x2940U},{16U,0x2960U},{20U,0x2980U},{24U,0x2a00U},{28U,0x2a40U},{48U,0x2b00U},{52U,0x2b80U},{56U,0x2b20U}})
                    store_native_word(ram,0xc300+offset,target);
                const std::string device_name="fixture",file_path="fixture7:QR";
                std::copy(device_name.begin(),device_name.end(),ram.begin()+0xc040); ram[0xc040+device_name.size()]=0;
                std::copy(file_path.begin(),file_path.end(),ram.begin()+0xc080); ram[0xc080+file_path.size()]=0;
                const auto call=[&](unsigned pc,unsigned a,unsigned b=0,unsigned c=0) {
                    auto cpu=initial(); cpu.pc=pc; cpu.registers[4]=a; cpu.registers[5]=b; cpu.registers[6]=c;
                    const auto stop=run_native(nt_iop_generated_run,cpu,ram,128,&router);
                    check(stop.reason==NativeStop::unmapped_code && cpu.pc==0x5000 && !cpu.native_call_owner,
                          "Compiled driver file operation did not restore its caller");
                    return cpu.registers[2];
                };
                const auto driver_seek64=[&](unsigned handle,std::int64_t offset,unsigned whence) {
                    auto cpu=initial(); cpu.pc=0x23e4; cpu.registers[4]=handle; cpu.registers[5]=0xdeadbeef;
                    const auto bits=static_cast<std::uint64_t>(offset);
                    cpu.registers[6]=static_cast<std::uint32_t>(bits); cpu.registers[7]=static_cast<std::uint32_t>(bits>>32);
                    cpu.registers[29]=0x8000c460; store_native_word(ram,0xc470,whence);
                    cpu.load_register=3; cpu.load_value=0xabcdef01;
                    const auto stop=run_native(nt_iop_generated_run,cpu,ram,128,&router);
                    check(stop.reason==NativeStop::unmapped_code && cpu.pc==0x5000 &&
                          !cpu.load_register && !cpu.native_call_owner && cpu.registers[29]==0x8000c460,
                          "Compiled wide seek lost its continuation, stack or pending load");
                    return std::bit_cast<std::int64_t>(std::uint64_t(cpu.registers[2])|(std::uint64_t(cpu.registers[3])<<32));
                };
                const auto control2=[&](unsigned handle,unsigned input,unsigned input_size,unsigned output,unsigned output_size,
                                        NativeStop expected=NativeStop::unmapped_code,unsigned stack=0x8000c460) {
                    auto cpu=initial(); cpu.pc=0x2814; cpu.registers[4]=handle; cpu.registers[5]=0xfffffff9;
                    cpu.registers[6]=input; cpu.registers[7]=input_size; cpu.registers[29]=stack;
                    store_native_word(ram,0xc470,output); store_native_word(ram,0xc474,output_size);
                    const auto before=ram;
                    const auto stop=run_native(nt_iop_generated_run,cpu,ram,128,&router);
                    check(stop.reason==expected,"Compiled ioctl2 produced an unexpected stop");
                    if (expected==NativeStop::unmapped_code)
                        check(cpu.pc==0x5000 && !cpu.native_call_owner && cpu.registers[29]==stack,
                              "Compiled ioctl2 lost caller ownership or stack");
                    else check(!cpu.executed && ram==before,"Rejected ioctl2 changed state before dispatch");
                    return cpu.registers[2];
                };
                const auto devctl=[&](unsigned path,unsigned input,unsigned length,unsigned output,unsigned size,
                                        NativeStop expected=NativeStop::unmapped_code,unsigned stack=0x8000c460) {
                    auto cpu=initial(); cpu.pc=0x282c; cpu.registers[4]=path; cpu.registers[5]=0xfffffff9;
                    cpu.registers[6]=input; cpu.registers[7]=length; cpu.registers[29]=stack;
                    store_native_word(ram,0xc470,output); store_native_word(ram,0xc474,size);
                    const auto before=ram;
                    const auto stop=run_native(nt_iop_generated_run,cpu,ram,128,&router);
                    check(stop.reason==expected,"Compiled devctl produced an unexpected stop");
                    if (expected==NativeStop::unmapped_code)
                        check(cpu.pc==0x5000 && !cpu.native_call_owner && cpu.registers[29]==stack &&
                              load_native_word(ram,0xc470)==output && load_native_word(ram,0xc474)==size,
                              "Compiled devctl lost caller stack or arguments");
                    else check(!cpu.executed && ram==before,"Rejected devctl modified memory");
                    return cpu.registers[2];
                };
                const auto mount_call=[&](unsigned name,unsigned payload,unsigned size,unsigned flags,
                                          NativeStop expected=NativeStop::unmapped_code,unsigned stack=0x8000c460) {
                    auto cpu=initial(); cpu.pc=0x2f4c; cpu.registers[4]=0xc080; cpu.registers[5]=name;
                    cpu.registers[6]=flags; cpu.registers[7]=payload; cpu.registers[29]=stack;
                    store_native_word(ram,0xc470,size);
                    const auto before=ram;
                    const auto stop=run_native(nt_iop_generated_run,cpu,ram,128,&router);
                    check(stop.reason==expected,"Mount produced an unexpected stop");
                    if (expected==NativeStop::unmapped_code)
                        check(cpu.pc==0x5000 && !cpu.native_call_owner && cpu.registers[29]==stack &&
                              load_native_word(ram,0xc470)==size,"Mount lost its caller stack/continuation");
                    else check(!cpu.executed && ram==before,"Rejected mount changed state");
                    return cpu.registers[2];
                };
                const auto free_bytes=[&] { return heap.invoke({8,{},NativeServiceKind::sysmem},ram).value; };
                const auto initial_free=free_bytes();
                store_native_word(ram,0xc324,0x2e00); store_native_word(ram,0xc328,0x2e40); store_native_word(ram,0xc32c,0x2880);
                store_native_word(ram,0xc35c,0x2e80);
                store_native_word(ram,0xc350,0x2a80);
                store_native_word(ram,0xc364,0x2f80);
                store_native_word(ram,0xc344,0x2fc0); store_native_word(ram,0xc360,0x2fc0);
                store_native_word(ram,0xc348,0x2e00); store_native_word(ram,0xc34c,0x2e40); store_native_word(ram,0xc354,0x2880);
                store_native_word(ram,0xc320,0x2d80); store_native_word(ram,0xc368,0x2da0);
                check(call(0x281c,0xc000)==0,"File driver initialization failed");
                check(call(0x23bc,0x8000c080)=='Q'+7 && call(0x23c4,0xc080)==0 &&
                      call(0x23cc,0xc080)==0xffffffff && free_bytes()==initial_free && !descriptors.find(4),
                      "Compiled path calls lost unit/path, callback result or temporary ownership");
                auto removing=initial(); removing.pc=0x23bc; removing.registers[4]=0xc080;
                check(run_native(nt_iop_generated_run,removing,ram,1,&router).reason==NativeStop::budget &&
                      removing.pc==0x2e00 && descriptors.find(4) && free_bytes()<initial_free,
                      "Path callback did not retain its temporary record");
                auto deleting=initial(); deleting.pc=0x2824; deleting.registers[4]=0xc040;
                check(run_native(nt_iop_generated_run,deleting,ram,1,&router).reason==NativeStop::import_request,
                      "Driver removed during an active path callback");
                check(nt_iop_generated_run(removing,ram,128).reason==NativeStop::unmapped_code &&
                      removing.pc==native_service_return,"Path callback failed to reach its return");
                check(run_native(nt_iop_generated_run,removing,std::span(ram).first(0xd100),1,&router).reason==NativeStop::address_fault &&
                      descriptors.find(4),"Path cleanup discarded ownership on a truncated heap");
                check(run_native(nt_iop_generated_run,removing,ram,1,&router).reason==NativeStop::unmapped_code &&
                      removing.pc==0x5000 && removing.registers[2]=='Q'+7 && free_bytes()==initial_free && !descriptors.find(4),
                      "Retried path cleanup lost its result or leaked ownership");
                check(devctl(0xc080,0xffffffff,1,0xffffffff,20)==static_cast<std::uint32_t>(-48),
                      "Legacy driver accepted devctl");
                for (const auto pc : {0x2f14U,0x2f1cU,0x2f24U,0x2f2cU})
                    check(call(pc,0xc080,1)==static_cast<std::uint32_t>(-48) && free_bytes()==initial_free,
                          "Legacy device accepted an extended path operation");
                check(call(0x2f34,0xc080,0xc089)==static_cast<std::uint32_t>(-48) &&
                      call(0x2f3c,0xc080,0xc089)==static_cast<std::uint32_t>(-48),
                      "Legacy device accepted two-path operations");
                check(mount_call(0xffffffff,0xffffffff,99,0)==static_cast<std::uint32_t>(-48),
                      "Legacy device accepted mount");
                const auto driver_fd=call(0x1614,0x8000c080,1);
                check(driver_seek64(0x7fffffff,0,0)==-9 && driver_seek64(driver_fd,0,0)==-48,"Legacy device accepted an extended seek or lost signed error width");
                const auto record=descriptors.find(driver_fd);
                check(call(0x2f44,driver_fd)==0x10 && call(0x2f44,fd)==0x10000010 &&
                      call(0x2f44,0x7fffffff)==static_cast<std::uint32_t>(-9),
                      "Device type query lost descriptor ownership or invalid-handle errors");
                store_native_word(ram,0xc004,0x10000010);
                auto changed_type=initial(); changed_type.pc=0x2f44; changed_type.registers[4]=driver_fd;
                check(run_native(nt_iop_generated_run,changed_type,ram,1,&router).reason==NativeStop::import_request &&
                      !changed_type.executed,"Device query silently accepted an unsupported type mutation");
                store_native_word(ram,0xc004,0x10);
                check(driver_fd==4 && record && record->owner==&driver_files &&
                      load_native_word(ram,record->local)==1 && load_native_word(ram,record->local+4)==7 &&
                      load_native_word(ram,record->local+8)==0xc000 && load_native_word(ram,record->local+12)==0xc089,
                      "Compiled open lost device unit/path, file ABI, or collided with ISO handle");
                check(call(0x1624,driver_fd,0xc200,1)==1 && ram[0xc200]=='Q' &&
                      call(0x1624,driver_fd,0xffffffff,0)==0 && file_call(8,fd,0,1)==2051,
                      "Compiled read did not use private state or changed another mount's cursor");
                check(call(0x23b4,driver_fd,0xffffffff,1)==static_cast<std::uint32_t>(-9),
                      "Compiled write ignored a read-only handle");
                check(call(0x162c,driver_fd,1,1)==1 && call(0x1624,driver_fd,0xc200,1)==1 && ram[0xc200]=='R' &&
                      call(0x162c,driver_fd,0xffffffff,1)==0xffffffffU && call(0x1624,driver_fd,0xc200,1)==1 && ram[0xc200]=='Q',
                      "Compiled seek lost its signed offset/return or private cursor update");
                check(call(0x162c,driver_fd,99,3)==static_cast<std::uint32_t>(-22) &&
                      call(0x1624,driver_fd,0xc200,1)==1 && ram[0xc200]=='Q',
                      "Invalid seek origin reached the compiled driver");
                auto rejected=initial(); rejected.pc=0x1624; rejected.registers[4]=driver_fd;
                rejected.registers[5]=0x1000; rejected.registers[6]=1;
                check(run_native(nt_iop_generated_run,rejected,ram,128,&router).reason==NativeStop::code_write && !rejected.executed,
                      "Compiled read accepted a code destination");
                rejected=initial(); rejected.pc=0x2824; rejected.registers[4]=0xc040;
                check(run_native(nt_iop_generated_run,rejected,ram,128,&router).reason==NativeStop::import_request &&
                      !rejected.executed && drivers.size()==1,"Driver was removed while its file was open");
                check(call(0x161c,driver_fd)==static_cast<std::uint32_t>(-7) && !descriptors.find(driver_fd) &&
                      free_bytes()==initial_free,"Failed close leaked the file record or descriptor");
                check(call(0x2f44,driver_fd)==static_cast<std::uint32_t>(-9),"Closed descriptor still reported a device type");
                const auto disc_fd=file_call(4,0x2000,1);
                check(disc_fd==driver_fd && file_call(5,disc_fd)==0,"ISO mount could not reuse a released driver descriptor");
                store_native_word(ram,0xc008,0xffffffff);
                check(call(0x1614,0xc080,1)==0xffffffffU && free_bytes()==initial_free && !descriptors.find(4),
                      "Failed compiled open leaked allocation or published its handle");
                store_native_word(ram,0xc008,1);
                check(control2(0x7fffffff,0,0,0,0)==static_cast<std::uint32_t>(-9),"ioctl2 accepted a missing handle");
                const auto write_only=call(0x1614,0xc080,2);
                check(write_only==4 && call(0x1624,write_only,0xffffffff,1)==static_cast<std::uint32_t>(-9) &&
                      call(0x161c,write_only)==static_cast<std::uint32_t>(-7),"Read ignored the descriptor access mode");
                const auto writable=call(0x1614,0xc080,3);
                ram[0xc220]='X';
                check(writable==4 && call(0x23b4,writable,0x8000c220,1)==1 &&
                      call(0x1624,writable,0xc200,1)==1 && ram[0xc200]=='X' &&
                      call(0x23b4,writable,0xffffffff,0)==0,
                      "Compiled write did not consume its source or preserve driver state");
                auto invalid_write=initial(); invalid_write.pc=0x23b4; invalid_write.registers[4]=writable;
                invalid_write.registers[5]=0xffff; invalid_write.registers[6]=2;
                const auto before_write=ram;
                check(run_native(nt_iop_generated_run,invalid_write,ram,128,&router).reason==NativeStop::address_fault &&
                      !invalid_write.executed && ram==before_write &&
                      call(0x23b4,writable,0xc220,0x100001)==static_cast<std::uint32_t>(-22),
                      "Invalid compiled-write source changed state or reached the callback");
                check(call(0x161c,writable)==static_cast<std::uint32_t>(-7),"Read/write file close failed");
                const auto control_file=call(0x1614,0xc080,1);
                check(call(0x23ec,control_file,123,0xfffffffd)==0xfffffffd &&
                      control2(control_file,0xffffffff,99,0xffffffff,99)==static_cast<std::uint32_t>(-48),
                      "Legacy control lost its scalar argument/result or accepted ioctl2");
                check(call(0x161c,control_file)==static_cast<std::uint32_t>(-7),"Control file cleanup failed");
                const auto directory=call(0x239c,0xc080),other_directory=call(0x239c,0xc080);
                check(directory==4 && other_directory==5 && call(0x161c,directory)==static_cast<std::uint32_t>(-9) &&
                      call(0x1624,directory,0xc500,1)==static_cast<std::uint32_t>(-21),
                      "Compiled directory handles allowed file-only operations");
                for (const auto output : {0x1000U,0xff00U,0xc501U}) {
                    auto read=initial(); read.pc=0x23ac; read.registers[4]=directory; read.registers[5]=output;
                    const auto before=ram;
                    check(run_native(nt_iop_generated_run,read,ram,128,&router).reason==
                          (output==0x1000 ? NativeStop::code_write : NativeStop::address_fault) &&
                          !read.executed && ram==before,"Invalid compiled directory output reached its callback");
                }
                std::fill_n(ram.begin()+0xc500,300,0xdd);
                check(call(0x23ac,directory,0xc500)==1 && load_native_word(ram,0xc500)==0x14 &&
                      load_native_word(ram,0xc508)==17 && load_native_word(ram,0xc628)==0 &&
                      std::string(reinterpret_cast<const char*>(ram.data()+0xc528))=="entry" &&
                      call(0x23ac,other_directory,0xc700)==1,
                      "Compiled directory read lost its record or shared a cursor");
                const auto before_eof=ram;
                check(call(0x23ac,directory,0xc500)==0 && ram==before_eof &&
                      call(0x23a4,directory)==0 && call(0x23a4,other_directory)==0 && free_bytes()==initial_free,
                      "Compiled directory EOF or close lost state/ownership");
                store_native_word(ram,0xc330,0x2940); store_native_word(ram,0xc008,0xffffffff);
                check(call(0x239c,0xc080)==0xffffffffU && free_bytes()==initial_free && !descriptors.find(4),
                      "Failed directory open leaked its file record or descriptor");
                store_native_word(ram,0xc330,0x2b00); store_native_word(ram,0xc008,1);
                check(call(0x2824,0xc040)==0 && !drivers.size() && free_bytes()==initial_free,
                      "File driver teardown or heap accounting failed");
                store_native_word(ram,0xc004,0x10000010);
                check(call(0x281c,0xc000)==0,"Extended device registration failed");
                ram[0xc600]=0x5a;
                check(devctl(0x8000c080,0xa000c600,1,0xc620,20)==0xfffffff9 &&
                      load_native_word(ram,0xc620)=='X' && load_native_word(ram,0xc624)==7 &&
                      load_native_word(ram,0xc628)==1 && load_native_word(ram,0xc62c)==20 &&
                      load_native_word(ram,0xc630)==0x5a && free_bytes()==initial_free && !descriptors.find(4),
                      "Compiled devctl lost shifted arguments, relative path, unit or temporary ownership");
                devctl(0xc080,0xffff,2,0xc620,20,NativeStop::address_fault);
                devctl(0xc080,0xc600,1,0x1000,20,NativeStop::code_write);
                devctl(0xc080,0xc600,1,0xc620,20,NativeStop::address_fault,0xfffffff8);
                devctl(0xc080,0xc440,1,0xc620,20,NativeStop::import_request);
                check(devctl(0xc080,0xc600,0x100001,0xc620,20)==static_cast<std::uint32_t>(-22) &&
                      free_bytes()==initial_free && !descriptors.find(4),"Rejected devctl leaked temporary ownership");
                check(call(0x2f14,0x8000c080)==0 && call(0x2f1c,0xc080,123)==0 &&
                      call(0x2f1c,0xc080,0xfffffff9)==0xfffffff9 && call(0x2f24,0xc080)==0xffffffff &&
                      free_bytes()==initial_free && !descriptors.find(4),
                      "Extended path operations lost success normalization, signed flags/results or cleanup");
                ram[0xc650]=0; ram[0xc651]=0xa5;
                check(call(0x2f2c,0x8000c080,0xa000c650,1)==1 && ram[0xc650]=='X' && ram[0xc651]==0xa5 &&
                      call(0x2f2c,0xc080,0xffffffff,0)==0 && free_bytes()==initial_free && !descriptors.find(4),
                      "Compiled readlink lost its byte count, extent, zero length or temporary ownership");
                for (const auto output : {0x1000U,0xffffU}) {
                    auto link=initial(); link.pc=0x2f2c; link.registers[4]=0xc080;
                    link.registers[5]=output; link.registers[6]=2;
                    const auto before=ram;
                    check(run_native(nt_iop_generated_run,link,ram,128,&router).reason==
                          (output==0x1000 ? NativeStop::code_write : NativeStop::address_fault) &&
                          !link.executed && ram==before && free_bytes()==initial_free && !descriptors.find(4),
                          "Invalid readlink destination changed state");
                }
                check(call(0x2f2c,0xc080,0xc650,0x100001)==static_cast<std::uint32_t>(-22),
                      "Readlink accepted an oversized output");
                store_native_word(ram,0xc364,0x2880);
                check(call(0x2f2c,0xc080,0xc650,1)==0xffffffff && free_bytes()==initial_free && !descriptors.find(4),
                      "Readlink error lost its result or leaked ownership");
                store_native_word(ram,0xc364,0x2f80);
                const auto destination=[&](const std::string& text) {
                    std::copy(text.begin(),text.end(),ram.begin()+0xc900); ram[0xc900+text.size()]=0;
                };
                for (const auto pc : {0x2f34U,0x2f3cU}) {
                    destination("Y");
                    check(call(pc,0x8000c080,0xa000c900)==static_cast<std::uint32_t>(-('X'+'Y')),
                          "Two-path callback lost a relative destination or negative result");
                    destination("fixture7:Z");
                    check(call(pc,0xc080,0xc900)==static_cast<std::uint32_t>(-('X'+'Z')),
                          "Two-path callback did not strip a matching device prefix");
                    for (const auto path : {"fixture8:Z","other7:Z"}) {
                        destination(path);
                        check(call(pc,0xc080,0xc900)==static_cast<std::uint32_t>(-18),
                              "Two-path call crossed a device or unit");
                    }
                    check(call(pc,0xc080,0)==static_cast<std::uint32_t>(-18),"Null second path was accepted");
                    std::fill_n(ram.begin()+0xc900,256,'A');
                    check(call(pc,0xc080,0xc900)==static_cast<std::uint32_t>(-36),"Unterminated second path was accepted");
                    auto bad=initial(); bad.pc=pc; bad.registers[4]=0xc080; bad.registers[5]=0xffffffff;
                    const auto before=ram;
                    check(run_native(nt_iop_generated_run,bad,ram,128,&router).reason==NativeStop::address_fault &&
                          !bad.executed && ram==before && free_bytes()==initial_free && !descriptors.find(4),
                          "Invalid second path changed state or leaked resources");
                }
                store_native_word(ram,0xc344,0x2e00); destination("Y");
                check(call(0x2f34,0xc080,0xc900)==0 && free_bytes()==initial_free && !descriptors.find(4),
                      "Rename success was not normalized or leaked resources");
                store_native_word(ram,0xc344,0x2fc0);
                destination("disk0:");
                check(mount_call(0x8000c900,0xa000c700,16,123)==0 && load_native_word(ram,0xc700)=='X' &&
                      load_native_word(ram,0xc704)=='d' && load_native_word(ram,0xc708)==7 && load_native_word(ram,0xc70c)==16 &&
                      mount_call(0xc900,0xc700,16,0xfffffff9)==0xfffffff9 && free_bytes()==initial_free && !descriptors.find(4),
                      "Compiled mount lost arguments, normalization or temporary cleanup");
                mount_call(0xffffffff,0xc700,16,0,NativeStop::address_fault);
                mount_call(0xc900,0xffff,16,0,NativeStop::address_fault);
                mount_call(0xc900,0xc700,16,0,NativeStop::address_fault,0xfffffff8);
                mount_call(0xc900,0xc440,16,0,NativeStop::import_request);
                check(mount_call(0xc900,0xc700,0x100001,0)==static_cast<std::uint32_t>(-22),
                      "Mount accepted an oversized payload");
                store_native_word(ram,0xc350,0x2880);
                check(mount_call(0,0xffffffff,0,0)==0xffffffff && free_bytes()==initial_free && !descriptors.find(4),
                      "Mount dereferenced unused optional arguments or leaked ownership");
                store_native_word(ram,0xc350,0x2a80);
                check(call(0x23c4,0xc080)==0755 && free_bytes()==initial_free && !descriptors.find(4),
                      "Extended mkdir lost the legacy wrapper default permissions");
                store_native_word(ram,0xc338,0x2d00);
                const auto extended_directory=call(0x239c,0xc080);
                const auto directory_record=descriptors.find(extended_directory)->local;
                check(call(0x2f44,extended_directory)==0x10000010,"Extended directory type query failed");
                ram[0xc600]=0x5a; ram[0xc620]=0;
                check(control2(extended_directory,0x8000c600,1,0xa000c620,1)==0xfffffff9 && ram[0xc620]==0x5a &&
                      control2(extended_directory,0xffffffff,0,0xffffffff,0)==0xfffffff9,
                      "Extended directory control lost payload, zero-length semantics or signed return");
                control2(extended_directory,0xffff,2,0xc620,1,NativeStop::address_fault);
                control2(extended_directory,0xc600,1,0xffff,2,NativeStop::address_fault);
                control2(extended_directory,0xc600,1,0x1000,1,NativeStop::code_write);
                control2(extended_directory,0xc600,1,0xc620,1,NativeStop::address_fault,0xfffffff8);
                check(control2(extended_directory,0xc600,0x100001,0xc620,1)==static_cast<std::uint32_t>(-22),
                      "ioctl2 accepted an oversized input");
                check(call(0x23ec,extended_directory,7,0xffffffff)==0xffffffff,
                      "Directory ioctl lost its scalar parameter");
                store_native_word(ram,directory_record+12,0); // Reset synthetic cursor after control commands.

                const auto directory_free=free_bytes();
                check(extended_directory==4 && load_native_word(ram,directory_record)==8,
                      "Extended directory did not receive its public open-mode flag");
                std::fill_n(ram.begin()+0xc700,300,0xa5); ram[0xc82c]=0x7c;
                auto extended=initial(); extended.pc=0x23ac; extended.registers[4]=extended_directory; extended.registers[5]=0xc700;
                check(run_native(nt_iop_generated_run,extended,ram,1,&router).reason==NativeStop::budget &&
                      extended.pc==0x2d00 && extended.registers[5]%64==0 && extended.registers[5]!=0xc700 &&
                      load_native_word(ram,directory_record+12)==0 && free_bytes()<directory_free,
                      "Extended dread did not allocate an aligned isolated output record");
                check(nt_iop_generated_run(extended,ram,128).reason==NativeStop::unmapped_code &&
                      extended.pc==native_service_return && load_native_word(ram,directory_record+12)==1,
                      "Extended directory callback did not complete exactly one entry");
                check(run_native(nt_iop_generated_run,extended,std::span(ram).first(0xc800),1,&router).reason==NativeStop::address_fault &&
                      extended.pc==native_service_return && free_bytes()<directory_free,
                      "Directory completion did not retain a retryable continuation after output fault");
                check(run_native(nt_iop_generated_run,extended,ram,1,&router).reason==NativeStop::unmapped_code &&
                      extended.pc==0x5000 && extended.registers[2]==1 && free_bytes()==directory_free &&
                      load_native_word(ram,0xc700)==0x17 && load_native_word(ram,0xc708)==17 &&
                      load_native_word(ram,0xc704)==0 && load_native_word(ram,0xc724)==0 &&
                      std::string(reinterpret_cast<const char*>(ram.data()+0xc728))=="entry" &&
                      load_native_word(ram,0xc828)==0xa5a5a5a5 && ram[0xc82c]==0x7c,
                      "Extended-to-legacy directory conversion lost fields, private data or bounds");
                const auto extended_eof=ram;
                check(call(0x23ac,extended_directory,0xc700)==0 &&
                      std::equal(ram.begin()+0xc700,ram.begin()+0xc82d,extended_eof.begin()+0xc700) && free_bytes()==directory_free,
                      "Extended directory EOF overwrote caller output or leaked temporary storage");
                store_native_word(ram,0xc338,0x2880);
                check(call(0x23ac,extended_directory,0xc700)==0xffffffffU &&
                      std::equal(ram.begin()+0xc700,ram.begin()+0xc82d,extended_eof.begin()+0xc700) && free_bytes()==directory_free,
                      "Extended directory error overwrote caller output or leaked temporary storage");
                store_native_word(ram,0xc338,0x2d00);
                const auto occupied=heap.invoke({4,{0,directory_free,0,0},NativeServiceKind::sysmem},ram).value;
                check(occupied && call(0x23ac,extended_directory,0xc700)==static_cast<std::uint32_t>(-12) &&
                      load_native_word(ram,directory_record+12)==1,"Scratch allocation failure executed directory callback");
                heap.invoke({5,{occupied,0,0,0},NativeServiceKind::sysmem},ram);
                check(call(0x23a4,extended_directory)==0 && free_bytes()==initial_free,
                      "Extended directory close leaked retained storage");
                store_native_word(ram,0xc358,0x2c80);
                const auto extended_fd=call(0x1614,0xc080,1);
                const auto extended_record=descriptors.find(extended_fd)->local;
                check(driver_seek64(extended_fd,0x1234567800000005LL,1)==0x1234567800000005LL &&
                      load_native_word(ram,extended_record+12)==1 && driver_seek64(extended_fd,INT64_MIN,2)==INT64_MIN &&
                      load_native_word(ram,extended_record+12)==2,
                      "Compiled wide seek truncated its signed offset/result or lost the stack origin");
                check(driver_seek64(extended_fd,0,3)==-22 && load_native_word(ram,extended_record+12)==2,
                      "Invalid wide seek origin reached its callback or lost error sign extension");
                auto bad_wide=initial(); bad_wide.pc=0x23e4; bad_wide.registers[4]=extended_fd;
                bad_wide.registers[29]=0xfffffff8; bad_wide.load_register=3; bad_wide.load_value=77;
                const auto before_bad_wide=ram;
                check(run_native(nt_iop_generated_run,bad_wide,ram,128,&router).reason==NativeStop::address_fault &&
                      !bad_wide.executed && bad_wide.load_register==3 && ram==before_bad_wide,
                      "Invalid wide seek stack changed state before dispatch");
                store_native_word(ram,0xc358,0xc100); store_native_word(ram,0xc470,0);
                bad_wide.registers[29]=0xc460;
                check(run_native(nt_iop_generated_run,bad_wide,ram,128,&router).reason==NativeStop::unmapped_code &&
                      !bad_wide.executed && load_native_word(ram,extended_record+12)==2,
                      "Wide seek accepted an uncompiled function target");
                store_native_word(ram,0xc358,0x2c80);
                check(call(0x161c,extended_fd)==static_cast<std::uint32_t>(-7) && free_bytes()==initial_free,
                      "Wide seek damaged descriptor ownership");
                check(call(0x2824,0xc040)==0,"Extended device teardown failed");
            }

            check(file_call(8,fd,static_cast<std::uint32_t>(-3),2)==5000 && file_call(6,fd,0x3000,8)==3 &&
                  std::equal(payload.end()-3,payload.end(),ram.begin()+0x3000) && file_call(6,fd,0x3000,8)==0,
                  "Native seek, EOF or short read differs");
            check(file_call(8,fd,0,0)==0,"Native rewind failed");
            const auto seek64=[&](std::int64_t offset,std::uint32_t whence,std::uint32_t handle) {
                auto cpu=initial(); cpu.pc=0x23e4; cpu.registers[4]=handle; cpu.registers[5]=0xdeadbeef;
                const auto bits=static_cast<std::uint64_t>(offset);
                cpu.registers[6]=static_cast<std::uint32_t>(bits); cpu.registers[7]=static_cast<std::uint32_t>(bits>>32);
                cpu.registers[29]=0xcb80; store_native_word(ram,0xcb90,whence);
                cpu.load_register=3; cpu.load_value=0x12345678;
                const auto stop=run_native(nt_iop_generated_run,cpu,ram,1,&files);
                check(stop.reason==NativeStop::budget && cpu.executed==1 && cpu.pc==0x5000 && cpu.load_register==0,
                      "Compiled 64-bit seek did not retire and return through its caller");
                return std::bit_cast<std::int64_t>(std::uint64_t(cpu.registers[2])|(std::uint64_t(cpu.registers[3])<<32));
            };
            check(seek64(0x100000005LL,0,fd)==0x100000005LL && file_call(6,fd,0x4000,1)==0 &&
                  file_call(8,fd,0,1)==static_cast<std::uint32_t>(-22) && seek64(0,1,fd)==0x100000005LL &&
                  seek64(-5,1,fd)==0x100000000LL,
                  "64-bit seek truncated its offset/return or legacy seek changed a large cursor");
            check(seek64(INT64_MAX,0,fd)==INT64_MAX && seek64(1,1,fd)==-22 && seek64(INT64_MIN,1,fd)==-22 &&
                  seek64(0,1,fd)==INT64_MAX && seek64(-INT64_MAX,1,fd)==0 && seek64(-3,2,fd)==5000 &&
                  seek64(0,3,fd)==-22 && seek64(0,0,0x7fffffff)==-9 && seek64(0,0,fd)==0,
                  "64-bit seek overflow, negative offsets or signed error return differs");
            auto bad_seek=initial(); bad_seek.pc=0x23e4; bad_seek.registers[4]=fd; bad_seek.registers[29]=0xfffffff8;
            bad_seek.load_register=3; bad_seek.load_value=77;
            check(run_native(nt_iop_generated_run,bad_seek,ram,1,&files).reason==NativeStop::address_fault &&
                  !bad_seek.executed && bad_seek.load_register==3 && seek64(0,1,fd)==0,
                  "Invalid seek whence argument changed cursor or pending register state");
            const auto before_controls=ram;
            for (const auto ordinal : {9U,34U}) {
                check(file_call(ordinal,0x7fffffff,0xffffffff,0xffffffff)==static_cast<std::uint32_t>(-9) &&
                      file_call(ordinal,fd,0xffffffff,0xffffffff)==static_cast<std::uint32_t>(-5),
                      "Disc control lost descriptor validation or unknown-command error");
            }
            for (const auto command : {0x630dU,0x630eU,0x630fU})
                check(file_call(34,fd,command,0xffffffff)==static_cast<std::uint32_t>(-5),
                      "Streaming control accepted a non-streaming disc handle");
            for (const auto command : {0x6310U,0x6311U,0x6312U,0x10000U,0x10008U,0x10020U,0x10030U}) {
                auto control=initial(); control.pc=0x23ec; control.registers[4]=fd;
                control.registers[5]=command; control.registers[6]=0xffffffff;
                control.load_register=12; control.load_value=88;
                check(run_native(nt_iop_generated_run,control,ram,1,&files).reason==NativeStop::import_request &&
                      !control.executed && control.load_register==12,
                      "Known hardware control was silently accepted or retired");
            }
            check(ram==before_controls && file_call(8,fd,0,1)==0,
                  "Disc controls touched buffers or advanced the file cursor");
            const auto before_mutations=ram;
            for (const auto ordinal : {10U,11U,12U,17U,18U})
                check(file_call(ordinal,0x2000,0xffffffff,0xffffffff)==static_cast<std::uint32_t>(-30),
                      "Read-only disc mutation did not report a filesystem error");
            check(file_call(7,fd,0xffffffff,4)==static_cast<std::uint32_t>(-9) &&
                  file_call(7,fd,0xffffffff,0)==static_cast<std::uint32_t>(-9) && ram==before_mutations &&
                  nt::read_file(image.path)==disc_image && file_call(8,fd,0,1)==0,
                  "Rejected mutation changed memory, the disc image or the open-file cursor");
            path("host:must-not-create");
            for (const auto ordinal : {10U,11U,12U,17U,18U})
                check(file_call(ordinal,0x2000)==static_cast<std::uint32_t>(-19),
                      "Disc mutation accepted a foreign filesystem");
            path(R"(cdrom0:\DATA.BIN;1)");

            for (const auto destination : {0xffffU,0x1000U}) {
                auto cpu=initial(); cpu.pc=0x1624; cpu.registers[4]=fd; cpu.registers[5]=destination; cpu.registers[6]=4;
                cpu.load_register=12; cpu.load_value=123;
                const auto before=ram;
                const auto stop=run_native(nt_iop_generated_run,cpu,ram,1,&files);
                check(stop.reason==(destination==0x1000 ? NativeStop::code_write : NativeStop::address_fault) &&
                      !cpu.executed && cpu.load_register==12 && ram==before,"Invalid native read changed code, RAM or retirement state");
            }
            check(file_call(6,fd,0x3000,1)==1 && ram[0x3000]==payload[0],"Rejected read advanced the file cursor");
            check(file_call(6,fd,0xffffffff,0)==0 && file_call(6,fd,0x3000,0x80000000)==static_cast<std::uint32_t>(-22) &&
                  file_call(8,fd,static_cast<std::uint32_t>(-2),0)==static_cast<std::uint32_t>(-22),"Native file bounds differ");
            check(file_call(5,fd)==0 && file_call(5,fd)==static_cast<std::uint32_t>(-9) &&
                  file_call(6,fd,0x3000,1)==static_cast<std::uint32_t>(-9),"Closed descriptor remained usable");
            check(file_call(4,0x2000,2)==static_cast<std::uint32_t>(-30),"Disc opened for writing");
            path("host:file"); check(file_call(4,0x2000,1)==static_cast<std::uint32_t>(-19),"Native disc service accepted a host path");
            path("cdrom0:/../DATA.BIN"); check(file_call(4,0x2000,1)==static_cast<std::uint32_t>(-2),"Native disc service accepted traversal");
            path("cdrom0:/MISSING.BIN"); check(file_call(4,0x2000,1)==static_cast<std::uint32_t>(-2),"Missing disc file succeeded");
        }
        {
            // LoadStartModule may open a disc image. It must not report a module
            // id or write the guest result until an AOT entry actually returns.
            NativeModloadServices loader;
            std::vector<std::uint8_t> guest(0x10000,0);
            const auto place=[&](std::uint32_t address,std::string_view text) {
                std::copy(text.begin(),text.end(),guest.begin()+address);
                guest[address+text.size()]=0;
            };
            place(0x2000,"cdrom0:/MOD.IRX;1");
            place(0x2100,"host:MOD.IRX");
            place(0x2200,"alpha");
            store_native_word(guest,0x7000,0x11111111);
            NativeServiceRequest request;
            request.kind=NativeServiceKind::modload;
            request.selector=7;
            const auto ask=[&](std::uint32_t name,std::uint32_t arglen,std::uint32_t args,std::uint32_t result) {
                request.arguments={name,arglen,args,result};
                request.interrupt_context=false;
                return loader.invoke(request,guest);
            };
            const auto unchanged=[&] { return load_native_word(guest,0x7000)==0x11111111; };
            const auto null_name=ask(0,0,0,0x7000);
            check(null_name.status==NativeServiceStatus::returned && null_name.value==static_cast<std::uint32_t>(-201) &&
                  loader.load_decision()==-1 && unchanged(),
                  "Null LoadStartModule path classified a file or wrote the result");
            check(ask(0x2000,4,0x2200,0x7000).status==NativeServiceStatus::address_fault && loader.load_decision()==-1 && unchanged(),
                  "Unterminated LoadStartModule arguments opened a module");
            check(ask(0x2000,0,0,0x1250).status==NativeServiceStatus::code_write && loader.load_decision()==-1 && unchanged(),
                  "LoadStartModule accepted a result pointer in compiled code");
            const auto unmounted=ask(0x2000,0,0,0x7000);
            check(unmounted.status==NativeServiceStatus::returned && unmounted.value==static_cast<std::uint32_t>(-203) &&
                  loader.load_decision()==5 && unchanged(),
                  "LoadStartModule without a disc reported a module result");
            struct TemporaryImage {
                std::filesystem::path path=std::filesystem::temp_directory_path()/
                    ("anyps2-load-start-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".iso");
                ~TemporaryImage() { std::error_code ignored; std::filesystem::remove(path,ignored); }
            } image;
            const nt::Bytes payload{'m','o','d','u','l','e',1,2,3,4};
            nt::write_file(image.path,fixture::single_file_iso("MOD.IRX;1",payload));
            loader.attach_disc(image.path);
            const auto foreign=ask(0x2100,0,0,0x7000);
            check(foreign.status==NativeServiceStatus::returned && foreign.value==static_cast<std::uint32_t>(-201) &&
                  loader.load_decision()==4 && unchanged(),
                  "LoadStartModule accepted a non-disc path");
            place(0x2300,"cdrom0:/MISSING.IRX;1");
            const auto missing=ask(0x2300,0,0,0);
            check(missing.status==NativeServiceStatus::returned && missing.value==static_cast<std::uint32_t>(-203) &&
                  loader.load_decision()==3,
                  "Missing LoadStartModule image returned success");
            const auto opened=ask(0x2000,6,0x2200,0x7000);
            check(opened.status==NativeServiceStatus::unavailable && loader.load_decision()==1 &&
                  loader.load_path()=="cdrom0:/MOD.IRX;1" && loader.load_bytes()==payload.size() &&
                  loader.load_hash()==nt::fingerprint(payload) && unchanged(),
                  "Uncatalogued module image was started or its result was written");
            loader.attach_catalog({loader.load_hash()});
            check(ask(0x2000,6,0x2200,0x7000).status==NativeServiceStatus::unavailable && loader.load_decision()==2 && unchanged(),
                  "Catalogued module image was reported started before its entry ran");
            request.interrupt_context=true;
            const auto decision=loader.load_decision();
            check(loader.invoke(request,guest).status==NativeServiceStatus::unavailable && loader.load_decision()==decision && unchanged(),
                  "Interrupt LoadStartModule changed the catalog decision");
            request.interrupt_context=false;
            request.selector=8;
            request.arguments={0,0x2100,4,0x2200};
            const auto unknown=loader.invoke(request,guest);
            check(unknown.status==NativeServiceStatus::returned && unknown.value==static_cast<std::uint32_t>(-202) &&
                  loader.load_decision()==15 && unchanged(),
                  "StartModule of an unknown id wrote a result or started an entry");
        }
        {
            // A sealed entry is compiled code at its installed address. The file
            // hash selects it. The loader does not relocate or interpret the file.
            NativeHeapServices heap(0x10000,0xb000,0x10000);
            NativeModloadServices loader;
            loader.attach_heap(heap);
            std::vector<std::uint8_t> guest(0x10000,0);
            const auto place=[&](std::uint32_t address,std::string_view text) {
                std::copy(text.begin(),text.end(),guest.begin()+address);
                guest[address+text.size()]=0;
            };
            place(0x2000,"cdrom0:/ONE.IRX;1");
            place(0x2100,"cdrom0:/TWO.IRX;1");
            store_native_word(guest,0x7000,0x11111111);
            const nt::Bytes image(16,0);
            const nt::Bytes one{'o','n','e'};
            const nt::Bytes two{'t','w','o'};
            loader.attach_sealed({
                {nt::fingerprint(one),nt::fingerprint(image),0xb000,0xb288,16,0x600,0},
                {nt::fingerprint(two),nt::fingerprint(image),0xb000,0xb280,16,0x600,0},
            });
            struct TemporaryImage {
                std::filesystem::path path;
                explicit TemporaryImage(const char* name) : path(std::filesystem::temp_directory_path()/
                    (std::string("anyps2-sealed-")+name+"-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".iso")) {}
                ~TemporaryImage() { std::error_code ignored; std::filesystem::remove(path,ignored); }
            } first("one"), second("two");
            nt::write_file(first.path,fixture::single_file_iso("ONE.IRX;1",one));
            nt::write_file(second.path,fixture::single_file_iso("TWO.IRX;1",two));
            loader.attach_disc(first.path);
            const auto unchanged=[&] { return load_native_word(guest,0x7000)==0x11111111; };
            const auto run=[&](std::uint32_t name) {
                auto cpu=initial(); cpu.pc=0xb42c;
                cpu.registers[4]=name; cpu.registers[5]=0; cpu.registers[6]=0; cpu.registers[7]=0x7000;
                const auto result=run_native(nt_iop_generated_run,cpu,guest,128,&loader);
                return std::pair{result,cpu.registers[2]};
            };
            guest[0xb000]=1;
            const auto mismatch=run(0x2000);
            check(mismatch.first.reason==NativeStop::import_request && mismatch.second==99 &&
                  loader.load_decision()==9 && unchanged(),
                  "A sealed module with the wrong installed image was started");
            guest[0xb000]=0;
            const auto released=run(0x2000);
            check(released.first.reason==NativeStop::unmapped_code && released.first.pc==0x5000 &&
                  released.second==1 && load_native_word(guest,0x7000)==0x61 && loader.load_decision()==8,
                  "Released module entry did not return its value through the loader");
            const auto restarted=run(0x2000);
            check(restarted.first.reason==NativeStop::unmapped_code && restarted.second==2 &&
                  load_native_word(guest,0x7000)==0x61,
                  "Released module stayed resident");
            loader.attach_disc(second.path);
            const auto resident=run(0x2100);
            check(resident.first.reason==NativeStop::unmapped_code && resident.second==3 &&
                  load_native_word(guest,0x7000)==0x60 && loader.load_decision()==8,
                  "Resident module entry did not supply the module id");
            const auto duplicate=run(0x2100);
            check(duplicate.first.reason==NativeStop::unmapped_code &&
                  duplicate.second==static_cast<std::uint32_t>(-206) &&
                  load_native_word(guest,0x7000)==0x60 && loader.load_decision()==10,
                  "Second start of a resident module ran the entry again");
            NativeServiceRequest started;
            started.kind=NativeServiceKind::modload;
            started.selector=8;
            started.arguments={resident.second,0x2100,0,0x7000};
            const auto again=loader.invoke(started,guest);
            check(again.status==NativeServiceStatus::returned && again.value==static_cast<std::uint32_t>(-206) &&
                  loader.load_decision()==14 && load_native_word(guest,0x7000)==0x60,
                  "StartModule ran an entry that was already started");
            started.arguments={1,0,0,0x7000};
            const auto released_id=loader.invoke(started,guest);
            check(released_id.status==NativeServiceStatus::returned &&
                  released_id.value==static_cast<std::uint32_t>(-202) && loader.load_decision()==15 &&
                  load_native_word(guest,0x7000)==0x60,
                  "StartModule revived a released module id");
            loader.attach_disc(first.path);
            const auto occupied=run(0x2000);
            check(occupied.first.reason==NativeStop::unmapped_code &&
                  occupied.second==static_cast<std::uint32_t>(-205) &&
                  load_native_word(guest,0x7000)==0x60 && loader.load_decision()==13,
                  "Overlapping sealed image was started");
        }
        {
            // A held span stays out of ordinary allocation and is claimed only by
            // mode 2 at that base. LoadStartModule then runs the compiled entry.
            std::vector<std::uint8_t> arena(0x10000,0);
            NativeHeapServices reserved(0x10000,0x6000,0x9000);
            check(reserved.hold(0x6000,1) && !reserved.hold(0x6000,256) && !reserved.hold(0x5000,256),
                  "Hold failed or overlapped a held or reserved span");
            const auto mode0=reserved.invoke({4,{0,1,0},NativeServiceKind::sysmem},arena);
            const auto mode1=reserved.invoke({4,{1,1,0},NativeServiceKind::sysmem},arena);
            const auto mode2=reserved.invoke({4,{2,1,0x6000},NativeServiceKind::sysmem},arena);
            const auto allocated=reserved.invoke({4,{2,256,0x6000},NativeServiceKind::sysmem},arena);
            check(mode0.status==NativeServiceStatus::returned && mode0.value==0x6100 && mode1.value==0x8f00 &&
                  mode2.value==0x6000 && allocated.value==0 && !reserved.hold(0x6100,1),
                  "Mode 0 or mode 1 consumed a held span, or mode 2 did not return that base");
            NativeHeapServices heap(0x10000,0xb000,0x10000);
            NativeModloadServices loader;
            loader.attach_heap(heap);
            std::vector<std::uint8_t> guest(0x10000,0);
            const auto place=[&](std::uint32_t address,std::string_view text) {
                std::copy(text.begin(),text.end(),guest.begin()+address);
                guest[address+text.size()]=0;
            };
            place(0x2000,"cdrom0:/MOD.IRX;1");
            store_native_word(guest,0x7000,0x11111111);
            const nt::Bytes image(16,0);
            const nt::Bytes file{'s','e','a','l'};
            std::copy(image.begin(),image.end(),guest.begin()+0xb000);
            check(heap.hold(0xb000,0x600),"Sealed image hold failed");
            struct TemporaryImage {
                std::filesystem::path path=std::filesystem::temp_directory_path()/
                    ("anyps2-sealed-hold-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".iso");
                ~TemporaryImage() { std::error_code ignored; std::filesystem::remove(path,ignored); }
            } image_file;
            nt::write_file(image_file.path,fixture::single_file_iso("MOD.IRX;1",file));
            loader.attach_disc(image_file.path);
            loader.attach_catalog({nt::fingerprint(file)});
            NativeServiceRequest request;
            request.kind=NativeServiceKind::modload;
            request.selector=7;
            request.arguments={0x2000,0,0,0x7000};
            const auto catalogued=loader.invoke(request,guest);
            check(catalogued.status==NativeServiceStatus::unavailable && loader.load_decision()==2 &&
                  load_native_word(guest,0x7000)==0x11111111,
                  "Catalog hashes without attach_sealed did not stay at decision 2");
            loader.attach_sealed({{nt::fingerprint(file),nt::fingerprint(image),0xb000,0xb288,16,0x600,0}});
            auto cpu=initial(); cpu.pc=0xb42c;
            cpu.registers[4]=0x2000; cpu.registers[5]=0; cpu.registers[6]=0; cpu.registers[7]=0x7000;
            const auto called=run_native(nt_iop_generated_run,cpu,guest,1,&loader);
            check(called.reason==NativeStop::budget && cpu.pc==0xb288 && loader.load_decision()==7,
                  "Held LoadStartModule did not reach decision 7 at the compiled entry");
            const auto finished=run_native(nt_iop_generated_run,cpu,guest,128,&loader);
            check(finished.reason==NativeStop::unmapped_code && finished.pc==0x5000 && cpu.registers[2]==1 &&
                  loader.load_decision()==8 && load_native_word(guest,0x7000)==0x61,
                  "Held sealed entry did not run and finish through decision 8");
            // A nonresident return releases ownership, not its immutable AOT
            // address range. Ordinary argument/stack allocations must skip it.
            const auto free_after=heap.invoke({8,{},NativeServiceKind::sysmem},guest).value;
            const auto scratch=heap.invoke({4,{0,0x100,0},NativeServiceKind::sysmem},guest);
            check(free_after==0x4a00 && scratch.value==0xb600 &&
                  heap.invoke({4,{2,0x600,0xb000},NativeServiceKind::sysmem},guest).value==0xb000 &&
                  heap.invoke({5,{0xb000},NativeServiceKind::sysmem},guest).value==0,
                  "Unloaded AOT image became writable heap storage or could not be claimed again");
            for (const auto entry:{0xb280U,0xb288U}) {
                NativeHeapServices scheduled_heap(0x10000,0xb000,0x10000);
                NativeInterruptServices interrupts(0xa000);
                NativeThreadServices workers(interrupts,&scheduled_heap);
                NativeModloadServices scheduled_loader;
                scheduled_loader.attach_heap(scheduled_heap);
                scheduled_loader.attach_scheduler(workers);
                scheduled_loader.attach_disc(image_file.path);
                scheduled_loader.attach_sealed({{nt::fingerprint(file),nt::fingerprint(image),0xb000,entry,16,0x600,0}});
                check(scheduled_heap.hold(0xb000,0x600),"Scheduled image hold failed");
                NativeServiceRouter services({&scheduled_loader,&workers,&interrupts});
                auto caller=initial(); caller.pc=0xb42c; caller.registers[31]=0xfffffffc;
                caller.registers[4]=0x2000; caller.registers[5]=0; caller.registers[6]=0;
                caller.registers[7]=0x7000; caller.registers[16]=123; caller.registers[29]=0x9000;
                const auto caller_id=workers.admit(caller);
                check(workers.invoke({14,{caller_id,64},NativeServiceKind::thbase},guest).value==0,
                      "Loader caller priority setup failed");
                store_native_word(guest,0x7000,0x11111111);
                check(workers.run(nt_iop_generated_run,guest,1,services).reason==NativeStop::budget &&
                      scheduled_loader.load_decision()==7 && load_native_word(guest,0x7000)==0x11111111,
                      "Module load reported completion before its compiled initializer ran");
                auto other=initial(); other.pc=0xb290; other.registers[31]=0xfffffffc;
                const auto other_id=workers.admit(other);
                check(workers.invoke({14,{other_id,48},NativeServiceKind::thbase},guest).value==0,
                      "Competing module worker priority setup failed");
                const auto initialized=workers.run(nt_iop_generated_run,guest,64,services);
                check(initialized.reason==NativeStop::unmapped_code && initialized.pc==0xfffffffc &&
                      scheduled_loader.load_decision()==8 && workers.state(other_id).executed==0 &&
                      workers.state(caller_id).executed==1 && workers.state(caller_id).registers[2]==1 &&
                      load_native_word(guest,0x7000)==(entry==0xb280 ? 0x60U : 0x61U),
                      "Module initializer borrowed its caller priority or woke the caller before completion");
                check(workers.run(nt_iop_generated_run,guest,64,services).reason==NativeStop::unmapped_code &&
                      workers.state(other_id).registers[2]==0x62 &&
                      workers.run(nt_iop_generated_run,guest,64,services).reason==NativeStop::unmapped_code &&
                      workers.state(caller_id).registers[16]==123 && workers.state(caller_id).registers[29]==0x9000 &&
                      workers.state(caller_id).registers[2]==1,
                      "Module completion lost the caller context or subsequent ready worker");
            }
        }
        std::cout<<"Compiled IOP LLVM execution passed; dmacman register checks use the diagnostic device library\n";
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
