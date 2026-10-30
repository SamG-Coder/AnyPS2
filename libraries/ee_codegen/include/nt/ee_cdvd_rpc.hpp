#pragma once
#include <nt/ee_rpc_service.hpp>
#include <nt/iso9660.hpp>
#include <array>
#include <cstring>
#include <memory>

namespace nt::ee {
// The public CD/DVD initialization endpoint owns a native ISO session. Other
// CD/DVD endpoints are registered separately only when implemented.
class NativeCdvdInitRpc final : public NativeRpcService {
public:
    explicit NativeCdvdInitRpc(std::filesystem::path image,std::uint32_t server=0x4000,std::uint32_t buffer=0x5000)
        : NativeRpcService(0x80000592U,server,buffer),image_(std::move(image)) {}
    bool initialized() const { return bool(disc_); }
    const std::string& error() const { return error_; }
    Stop call(Memory& memory,CodeOverlap protection,std::uint32_t method,
              std::span<const std::uint8_t> args,std::uint32_t destination,std::uint32_t size) override {
        if (method!=0) return Stop::unsupported_system;
        if (args.size()!=4 || (size!=4 && size!=16)) return Stop::invalid_state;
        const auto mode=le32(args,0);
        if (mode!=0 && mode!=1 && mode!=5) return Stop::unsupported_system;
        const auto valid=rpc_output(memory,protection,destination,size);
        if (valid!=Stop::budget) return valid;
        std::unique_ptr<disc::Iso9660> next;
        try {
            if (mode!=5) {
                next=std::make_unique<disc::Iso9660>(image_);
                // Normal initialization waits for usable media. Library-only
                // initialization opens the volume without traversing its root.
                if (mode==0) next->entries(next->root());
            }
        } catch (const std::runtime_error& e) {
            error_=e.what(); return Stop::unsupported_system;
        }
        // Public legacy response: success followed by absent module-version
        // metadata and disabled verbose logging. No Sony revision is claimed.
        const std::array<std::uint8_t,16> result{1};
        std::memcpy(memory.data+runtime::default_ram_address(destination),result.data(),size);
        disc_=std::move(next); error_.clear();
        return Stop::budget;
    }
private:
    std::filesystem::path image_;
    std::unique_ptr<disc::Iso9660> disc_;
    std::string error_;
};
}
