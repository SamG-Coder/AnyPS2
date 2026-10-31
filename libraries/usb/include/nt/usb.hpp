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
class Host {
public:
    virtual ~Host()=default;
    virtual bool scan(std::vector<Device>& devices)=0;
    virtual std::uint32_t error() const=0;
};
std::unique_ptr<Host> make_host();
}
