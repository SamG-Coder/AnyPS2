#include <nt/sif.hpp>
#include <algorithm>

namespace nt::hardware {
void Sif::initialize_commands() {
    if (commands_ready_ || reset_delay_ || reboot_) return;
    // This address is an allocation in our IOP HLE memory, not a retail BIOS address.
    registers_[1] = 0x1000;
    commands_ready_ = true;
    registers_[3] |= 0x30000; // SIF transport and command service initialized.
}
void Sif::dispatch_command(runtime::RamBus& ee) {
    const auto read = [&](unsigned offset) {
        if (offset + 4 > iop_packet_bytes_) throw runtime::MemoryFault("SIF command: truncated packet");
        std::uint32_t value = 0;
        for (unsigned byte = 0; byte < 4; ++byte)
            value |= std::uint32_t(iop_ram_[iop_packet_address_ + offset + byte]) << (byte * 8);
        return value;
    };
    const auto length = read(0) & 255;
    if (length < 16 || length > 112 || length > iop_packet_bytes_)
        throw runtime::MemoryFault("SIF command: invalid packet size");
    const auto command = read(8), option = read(12);
    if (command == 0x80000003U) {
        if (length < 24 || read(16)>79 || read(16) > length-24) throw runtime::MemoryFault("SIF reset: invalid argument extent");
        const auto start=iop_packet_address_+24, size=read(16);
        const std::string argument(iop_ram_.begin()+start,iop_ram_.begin()+start+size);
        if (read(20)!=0 || option || argument.find('\0')!=std::string::npos)
            throw runtime::MemoryFault("SIF reset: unsupported mode or embedded terminator");
        if (!argument.empty() && argument!="rom0:UDNL") {
            constexpr std::string_view loader="rom0:UDNL ";
            if (!argument.starts_with(loader)) throw runtime::MemoryFault("SIF reset: unsupported loader arguments");
            if (!fileio_) throw runtime::MemoryFault("SIF reset: reboot image requires a mounted disc");
            if (reset_pending_ || reboot_) throw runtime::MemoryFault("SIF reset: reset already pending");
            try {
                const auto image=fileio_->read_disc_file(argument.substr(loader.size()),16*1024*1024);
                reboot_=std::make_unique<iop::Reboot>(image,this);
            } catch (const std::exception& error) {
                throw runtime::MemoryFault(std::string("SIF reset: ")+error.what());
            }
        }
        if (reset_pending_) throw runtime::MemoryFault("SIF reset: reset already pending");
        reset_pending_=true;
        return;
    }
    const auto reply = [&](std::span<const std::uint32_t> words) {
        if (!replies_.empty()) throw runtime::MemoryFault("SIF RPC: reply queue busy");
        if (!ee_command_address_) throw runtime::MemoryFault("SIF RPC: EE receive address is unset");
        const auto bytes = (words.size() * 4 + 15) & ~std::size_t(15);
        std::vector<std::uint8_t> packet(bytes);
        for (unsigned i = 0; i < words.size(); ++i)
            for (unsigned byte = 0; byte < 4; ++byte) packet[i * 4 + byte] = static_cast<std::uint8_t>(words[i] >> (byte * 8));
        iop_install(0x1100,packet);
        iop_send(0x1100,ee_command_address_,static_cast<std::uint32_t>(bytes / 16));
    };
    if (command == 0x80000002U && option == 1) {
        // Acknowledge the dispatcher; services bind only when their backing exists.
        const std::array<std::uint32_t,6> response{24,0,0x80000001U,0,0,1};
        reply(response);
        rpc_ready_ = true;
        return;
    }
    if (command == 0x80000009U) {
        if (!rpc_ready_ || length < 36) throw runtime::MemoryFault("SIF RPC: invalid bind request");
        const bool available = fileio_ && (read(32) == 0x80000001U || read(32) == 0x80000592U);
        const bool cdvd = read(32) == 0x80000592U;
        const std::array<std::uint32_t,12> response{48,0,0x80000008U,0,
            read(16),read(20),read(24),read(28),0x80000009U,
            available ? (cdvd ? cdvd_server : fileio_server) : 0,
            available ? (cdvd ? cdvd_buffer : fileio_buffer) : 0,0};
        reply(response);
        last_rpc_bind_server_ = read(32);
        ++rpc_bind_requests_;
        return;
    }
    if (command == 0x8000000aU) {
        if (!rpc_ready_ || length < 56) throw runtime::MemoryFault("SIF RPC: invalid call request");
        dispatch_rpc(ee,std::span(iop_ram_).subspan(iop_packet_address_,length));
        return;
    }
    if (command == 0x80000000U || command == 0x80000002U) {
        if (length < 20) throw runtime::MemoryFault("SIF command: missing EE buffer");
        if (command == 0x80000002U && option)
            throw runtime::MemoryFault("SIF command: unknown initialization option");
        const auto address = read(16);
        if (address % 16 || address >= 32 * 1024 * 1024 || !address)
            throw runtime::MemoryFault("SIF command: invalid EE buffer");
        ee_command_address_ = address;
        if (command == 0x80000002U) registers_[2] |= 0x20000;
    } else if (command == 0x80000001U) {
        if (length < 24 || read(16) >= command_registers_.size())
            throw runtime::MemoryFault("SIF command: invalid software register");
        command_registers_[read(16)] = read(20);
    } else throw runtime::MemoryFault("SIF command: unsupported command " + nt::hex32(command));
}
void Sif::reset_step() {
    if (reboot_running_) { reboot_step(); return; }
    if (reset_delay_) {
        if (--reset_delay_==0) {
            initialize_commands();
            registers_[3] |= 0x40000; // The HLE service reset has completed.
        }
        return;
    }
    if (!reset_pending_ || !job_.empty() || (send_.control&0x100)) return;
    if (!to_iop_.empty() || iop_packet_) throw runtime::MemoryFault("SIF reset: trailing input after reset packet");
    if (receive_.control&0x100) throw runtime::MemoryFault("SIF reset: stop SIF0 before resetting services");
    reset_pending_=false;
    transmit_={}; to_ee_.clear(); replies_.clear();
    std::fill(iop_ram_.begin(),iop_ram_.end(),0);
    command_registers_.fill(0);
    if (reboot_) { registers_[1]=0; registers_[3]=0; }
    else registers_.fill(0);
    ee_command_address_=0; commands_ready_=rpc_ready_=cdvd_initialized_=false;
    if (fileio_) fileio_->reset();
    if (reboot_) {
        // EE-owned flags survive the IOP reset. Services remain unavailable until
        // the disc modules establish them; no HLE readiness delay is scheduled.
        reboot_running_=true;
        return;
    }
    ++reset_count_;
    // Diagnostic service startup steps, not a model of IOP firmware timing.
    reset_delay_=16;
}
void Sif::reboot_step() {
    if (!reboot_failure_.empty()) throw runtime::MemoryFault(reboot_failure_);
    try {
        // Bounded diagnostic scheduling lets EE instructions and SBUS writes run
        // between IOP slices. This ratio does not model hardware clock timing.
        const auto result=reboot_manager_ ? reboot_->run_manager(64) : reboot_->advance(64);
        if (result.reason==iop::StartupStop::budget) return;
        if (!reboot_manager_ && result.reason==iop::StartupStop::returned) {
            reboot_->prepare_manager();
            reboot_manager_=true;
            return;
        }
        std::string detail=reboot_manager_ ? "IOP module-manager" : "IOP memory bootstrap";
        if (result.reason==iop::StartupStop::fault) {
            detail+=" fault pc="+hex32(result.execution.pc)+" word="+hex32(result.execution.instruction);
            if (result.execution.address) detail+=" address="+hex32(*result.execution.address);
            if (result.execution.reason==iop::CpuStop::interrupt_pending)
                detail+=" interrupt_pending="+hex32(reboot_->pending_interrupts());
        } else detail+=" returned; service readiness is not implemented";
        throw runtime::MemoryFault(detail);
    } catch (const std::exception& error) {
        reboot_failure_="SIF reset: "+std::string(error.what())+"; iop_executed="+std::to_string(reboot_->executed());
        if (reboot_->intrman_registered()) reboot_failure_+="; native_intrman_registered=1";
        if (reboot_->dmacman_registered()) reboot_failure_+="; native_dmacman_registered=1";
        if (reboot_->heaplib_registered()) reboot_failure_+="; native_heaplib_registered=1";
        throw runtime::MemoryFault(reboot_failure_);
    }
}

}
