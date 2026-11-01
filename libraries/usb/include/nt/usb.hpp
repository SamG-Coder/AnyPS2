#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace nt::usb {
// A host snapshot owns descriptor bytes, not USB controller registers. Keys
// identify physical hub ports and are never used to select game behavior.
struct Device {
    std::wstring key;
    std::vector<std::uint8_t> descriptors;
    bool operator==(const Device&) const=default;
};
bool valid_descriptors(std::span<const std::uint8_t> bytes);
class ControlPipe {
public:
    virtual ~ControlPipe()=default;
    virtual bool read_descriptor(std::uint16_t value,std::uint16_t index,std::uint16_t length,
                                 std::vector<std::uint8_t>& bytes)=0;
    // Adopt an already active host configuration; unsupported changes must fail.
    virtual bool use_configuration(std::uint8_t configuration)=0;
    virtual bool use_interface(std::uint8_t interface_number,std::uint8_t alternate)=0;
    virtual std::uint32_t error() const=0;
};
class Host {
public:
    virtual ~Host()=default;
    virtual bool scan(std::vector<Device>& devices)=0;
    virtual std::unique_ptr<ControlPipe> open_control(const Device&) { return {}; }
    virtual std::uint32_t error() const=0;
};
std::unique_ptr<Host> make_host();
}
