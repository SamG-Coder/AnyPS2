#pragma once
#include <nt/iop_calls.hpp>
#include <nt/iop_devices.hpp>
#include <nt/iop_native_library.hpp>

namespace nt::iop {
enum class DmacmanStatus { unsupported, returned, unavailable };
struct DmacmanResult { DmacmanStatus status=DmacmanStatus::unsupported; std::uint32_t value=0; };
// Register reads, priority updates and channel enables. Ordinals 28 and 32 are
// unavailable and do not mutate devices or report a finished transfer.
DmacmanResult dmacman_invoke(Devices& devices,std::uint32_t ordinal,const std::array<std::uint32_t,4>& arguments);
class Dmacman final : public HostCalls {
public:
    Dmacman(Devices& devices,std::uint32_t base) : devices_(devices), library_("dmacman",0x102,36,base) {}
    static constexpr unsigned allocation_size() { return NativeLibrary::allocation_size(36); }
    void install(std::span<std::uint8_t> ram,Libraries& libraries) { library_.install(ram,libraries); }
    const elf::IopModule& module() const { return library_.module(); }
    bool owns(std::uint32_t pc,std::size_t size) const override { return library_.ordinal(pc,size).has_value(); }
    std::uint32_t registration_flag() const { return library_.registration_flag(); }
    CallResult invoke(std::uint32_t pc,const std::array<std::uint32_t,4>& arguments,
                      std::span<std::uint8_t> ram,bool interrupt_context) override;
private:
    Devices& devices_;
    NativeLibrary library_;
};
}
