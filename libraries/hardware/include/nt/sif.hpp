#pragma once
#include <nt/runtime.hpp>
#include <nt/fileio.hpp>
#include <nt/iop_reboot.hpp>
#include <deque>
#include <memory>

namespace nt::hardware {
// Diagnostic SIF transport with HLE services and a private partial reboot CPU.
// The reboot context cannot replace active services until startup completes.
class Sif : public iop::Sbus {
public:
    Sif();
    // A running reboot context borrows this object's SBUS interface.
    Sif(const Sif&)=delete;
    Sif& operator=(const Sif&)=delete;
    Sif(Sif&&)=delete;
    Sif& operator=(Sif&&)=delete;
    std::optional<std::uint32_t> read_sbus(std::uint32_t offset) const override;
    bool write_sbus(std::uint32_t offset,std::uint32_t value) override;
    static bool maps(std::uint32_t address);
    std::uint32_t read(std::uint32_t address) const;
    void write(std::uint32_t address, std::uint32_t value);
    void step(runtime::RamBus& ee, bool enabled);
    std::uint32_t take_completions();
    void iop_write_register(unsigned index, std::uint32_t value);
    std::uint32_t iop_read_register(unsigned index) const;
    void iop_install(std::uint32_t address, std::span<const std::uint8_t> bytes);
    std::span<const std::uint8_t> iop_memory() const { return iop_ram_; }
    void iop_send(std::uint32_t source, std::uint32_t destination, std::uint32_t quadwords);
    bool iop_send_pending() const { return transmit_.active || !replies_.empty(); }
    std::uint64_t iop_received_packets() const { return received_packets_; }
    std::uint64_t rpc_bind_requests() const { return rpc_bind_requests_; }
    std::uint32_t last_rpc_bind_server() const { return last_rpc_bind_server_; }
    void initialize_commands();
    void mount_disc(const std::filesystem::path& image);
    bool cdvd_initialized() const { return cdvd_initialized_; }
    std::uint64_t reset_count() const { return reset_count_; }
    const iop::Reboot* reboot_context() const { return reboot_.get(); }
    fileio::Statistics fileio_statistics() const { return fileio_ ? fileio_->statistics() : fileio::Statistics{}; }
    std::uint32_t submit(std::span<const runtime::SifTransfer> transfers);
    std::int32_t status(std::uint32_t id) const;
    std::uint32_t ee_command_address() const { return ee_command_address_; }
    std::uint32_t command_register(unsigned index) const { return command_registers_.at(index); }
private:
    struct Channel { std::uint32_t control = 0, address = 0, count = 0, tag = 0; bool end = false, paused = false; };
    struct Transfer { std::uint32_t source = 0, destination = 0, count = 0; bool active = false, tagged = false, end = true; };
    Channel receive_, send_;
    Transfer transmit_;
    std::deque<std::uint32_t> to_ee_, to_iop_;
    std::vector<std::uint8_t> iop_ram_;
    std::array<std::uint32_t,4> registers_{};
    std::uint32_t control_ = 0, completion_ = 0;
    std::uint32_t iop_destination_ = 0, iop_remaining_ = 0;
    bool iop_packet_ = false;
    std::uint32_t iop_packet_address_ = 0, iop_packet_bytes_ = 0;
    bool commands_ready_ = false;
    bool rpc_ready_ = false;
    bool reset_pending_ = false;
    unsigned reset_delay_ = 0;
    std::uint64_t reset_count_ = 0;
    std::unique_ptr<iop::Reboot> reboot_;
    bool reboot_running_=false, reboot_manager_=false;
    std::string reboot_failure_;
    void reset_step();
    void reboot_step();
    std::uint64_t rpc_bind_requests_ = 0;
    std::uint32_t last_rpc_bind_server_ = 0;
    std::unique_ptr<fileio::Service> fileio_;
    std::deque<fileio::Write> replies_;
    static constexpr std::uint32_t fileio_server = 0x1800, fileio_buffer = 0x2000;
    static constexpr std::uint32_t cdvd_server = 0x1840, cdvd_buffer = 0x3000;
    bool cdvd_initialized_ = false;
    std::uint32_t ee_command_address_ = 0;
    std::array<std::uint32_t,32> command_registers_{};
    std::uint64_t received_packets_ = 0;
    std::vector<runtime::SifTransfer> job_;
    std::size_t job_index_ = 0;
    std::uint32_t job_id_ = 0, job_offset_ = 0;
    bool job_tagged_ = false;
    std::uint64_t job_completion_ = 0;
    void receive_step(runtime::RamBus& ee);
    void send_step(runtime::RamBus& ee);
    void iop_step(runtime::RamBus& ee);
    void dispatch_command(runtime::RamBus& ee);
    void dispatch_rpc(runtime::RamBus& ee, std::span<const std::uint8_t> packet);
    void queue_rpc_reply(runtime::RamBus& ee, std::vector<fileio::Write> writes, std::span<const std::uint32_t> response);
    void reply_step();
    void job_step(runtime::RamBus& ee);
};
}
