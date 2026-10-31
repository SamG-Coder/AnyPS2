#pragma once
#include <nt/iop_native_heaplib.hpp>
#include <nt/iop_native_sysclib.hpp>
#include <nt/iop_native_prnt.hpp>
#include <nt/iop_native_libraries.hpp>
#include <nt/iop_native_boot_callbacks.hpp>
#include <nt/iop_native_disc.hpp>
#include <nt/iop_native_driver_files.hpp>
#include <nt/iop_native_semaphores.hpp>
#include <nt/iop_native_mailboxes.hpp>
#include <nt/iop_native_threads.hpp>
#include <nt/iop_native_alarms.hpp>
#include <nt/iop_native_sif_inbound.hpp>
#include <nt/iop_native_rpc_data.hpp>
#include <nt/iop_native_stdio.hpp>
#include <nt/iop_native_modload.hpp>
#include <nt/iop_native_dmacman.hpp>
#include <nt/iop_native_serial.hpp>
#include <nt/iop_native_cdvdman.hpp>
#include <nt/iop_native_secrman.hpp>
#include <nt/iop_native_archive.hpp>
#include <memory>

namespace nt::iop {
struct NativeContextLayout {
    std::uint32_t heap_begin;
    std::uint32_t heap_end=0x1f0000;
    std::uint32_t packet_buffer=0x1f0000;
    std::uint32_t interrupt_stack=0x1f7000;
};

// One instance per native IOP lifetime. The caller owns the data arena and EE
// destination for at least this lifetime. Destroy the context on reset rather
// than retaining stale handles, waiters, registrations or pending callbacks.
class NativeServiceContext final : public NativeServices {
public:
    NativeServiceContext(std::span<std::uint8_t> ram,NativeContextLayout layout,
                         std::vector<NativeLibrarySpec> exports,std::vector<NativeImportSpec> imports,
                         NativeStdioServices::Sink output,const std::filesystem::path& disc={},
                         NativeSifDestination* destination=nullptr,std::uint32_t command_buffer=0,
                         NativeSifCommandEndpoint::Delivery delivery={})
        : heap(checked_size(ram,layout,destination || !delivery),layout.heap_begin,layout.heap_end),heaplib(heap),boot_callbacks(heap),
          libraries(std::move(exports),std::move(imports)),archives(descriptors),driver_files(drivers,descriptors,heap),
          sif(destination),peer_(destination ? std::make_unique<NativeSifCommandEndpoint>(*destination,command_buffer,sif,std::move(delivery)) : nullptr),
          commands(peer_.get()),inbound(ram,layout.packet_buffer),callbacks({&alarms,&sif,&inbound}),
          interrupts(layout.interrupt_stack,&callbacks),sif_control(interrupts,&sif),threads(interrupts,&heap),
          rpc_servers(peer_.get(),&threads),rpc_data(peer_.get()),events(&threads),semaphores(&threads),mailboxes(&threads),
          system(events,boot,ram),command_session(events,system,sif_control,peer_.get()),stdio(std::move(output)),
          disc_service(disc.empty() ? nullptr : std::make_unique<NativeDiscServices>(disc,&descriptors,&events)),
          dmacman_(dmac_priority_),router(providers()),arena_(ram),packet_buffer_(layout.packet_buffer) {
        inbound.connect(commands,command_session,sif_control,&rpc_servers);
        modload.attach_heap(heap);
        if (!disc.empty()) modload.attach_disc(disc);
    }
    NativeServiceContext(const NativeServiceContext&)=delete;
    NativeServiceContext& operator=(const NativeServiceContext&)=delete;
    NativeSifCommandEndpoint* endpoint() const { return peer_.get(); }
    // Explicit native replacement for command-module startup. The EE owner must
    // publish its link first. This installs our reserved receive mailbox, but
    // neither acknowledges the EE handshake nor declares RPC/reset completion.
    NativeServiceReply initialize_commands() {
        if (!peer_) return {NativeServiceStatus::unavailable};
        if (sif_control.ready())
            return {sif_control.sub_address()==packet_buffer_ ? NativeServiceStatus::returned : NativeServiceStatus::unavailable};
        const auto initialized=sif_control.invoke({5,{},NativeServiceKind::sifman},arena_);
        if (initialized.status!=NativeServiceStatus::returned || !sif_control.ready())
            return {NativeServiceStatus::unavailable};
        sif_control.invoke({27,{packet_buffer_,0,0,0},NativeServiceKind::sifman},arena_);
        return {NativeServiceStatus::returned};
    }
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t> ram) override {
        if (ram.data()!=arena_.data() || ram.size()!=arena_.size()) return {NativeServiceStatus::address_fault};
        return router.invoke(request,ram);
    }

    NativeHeapServices heap;
    NativeHeapLibraryServices heaplib;
    NativeBootCallbacks boot_callbacks;
    NativeLibraryServices libraries;
    NativeSysclibServices sysclib;
    NativePrntServices prnt;
    NativeDriverServices drivers;
    NativeFileDescriptors descriptors;
    NativeArchiveServices archives;
    NativeDriverFiles driver_files;
    NativeBootServices boot; // Empty until the owner supplies real boot metadata.
    NativeModloadServices modload;
    NativeAlarmServices alarms;
    NativeCdvdmanServices cdvdman;
    NativeSecrmanServices secrman;
    NativeSifTransfers sif;
private:
    std::unique_ptr<NativeSifCommandEndpoint> peer_;
public:
    NativeSifCommands commands;
    NativeSifInbound inbound;
    NativeCallbackMux callbacks;
    NativeInterruptServices interrupts;
    NativeSifControl sif_control;
    NativeThreadServices threads;
    NativeRpcServers rpc_servers;
    NativeRpcDataServices rpc_data;
    NativeEventServices events;
    NativeSemaphoreServices semaphores;
    NativeMailboxServices mailboxes;
    NativeSystemServices system;
    NativeSifCommandSession command_session;
    NativeStdioServices stdio;
private:
    static std::uint32_t checked_size(std::span<std::uint8_t> ram,const NativeContextLayout& layout,bool valid_peer) {
        if (!valid_peer) throw std::invalid_argument("Native delivery requires an EE destination");
        if (ram.size()>0x200000 || layout.packet_buffer<layout.heap_end || layout.packet_buffer%16 ||
            std::uint64_t(layout.packet_buffer)+112>ram.size() ||
            layout.interrupt_stack<=std::uint64_t(layout.packet_buffer)+112 || layout.interrupt_stack>ram.size())
            throw std::invalid_argument("Native context requires disjoint bounded heap, packet and interrupt storage");
        return static_cast<std::uint32_t>(ram.size());
    }
    std::vector<NativeServices*> providers() {
        std::vector<NativeServices*> result{&boot_callbacks,&heaplib,&prnt,&sysclib,&drivers,&driver_files,&archives,&heap,&libraries,
            &boot,&modload,&interrupts,&events,&semaphores,&mailboxes,&threads,&system,&alarms,&sif,&sif_control,&stdio,
            &commands,&command_session,&rpc_servers,&rpc_data,&dmacman_,&cdvdman,&secrman,&serial_};
        if (disc_service) result.push_back(disc_service.get());
        return result;
    }
    std::unique_ptr<NativeDiscServices> disc_service;
    DmacPriorityState dmac_priority_;
    NativeDmacmanServices dmacman_;
    NativeSerialServices serial_;
    NativeServiceRouter router;
    std::span<std::uint8_t> arena_;
    std::uint32_t packet_buffer_;
};
}
