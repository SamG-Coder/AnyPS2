#pragma once
#include <cstdint>
#include <array>
#include <memory>
#include <optional>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace nt::usb {
// A host snapshot owns descriptor bytes, not USB controller registers. Keys
// identify physical hub ports and are never used to select game behavior.
struct Device {
    std::wstring key;
    std::vector<std::uint8_t> descriptors;
    std::map<std::uint8_t,std::vector<std::uint8_t>> hid_reports;
    bool operator==(const Device&) const=default;
};
bool valid_descriptors(std::span<const std::uint8_t> bytes);
struct Endpoint {
    std::uint8_t configuration=0,interface_number=0,alternate=0,interface_class=0,address=0,attributes=0;
    std::uint16_t packet_bytes=0;
};
// Resolve a descriptor boundary together with its owning interface.
bool describe_endpoint(const Device&,std::size_t offset,Endpoint&);
struct HidReportId {
    std::uint8_t type,id; // Input=1, output=2, feature=3.
    bool operator==(const HidReportId&) const=default;
};
std::optional<std::vector<HidReportId>> hid_report_ids(std::span<const std::uint8_t>);
// Compose host collections only when their report layouts cannot overlap.
bool merge_hid_reports(std::span<const std::vector<std::uint8_t>>,std::vector<std::uint8_t>&);
enum class TransferState { pending,complete,failed };
class InputPipe {
public:
    virtual ~InputPipe()=default;
    virtual bool submit(std::uint32_t capacity)=0;
    virtual TransferState poll(std::vector<std::uint8_t>& bytes)=0;
    virtual void cancel()=0;
    virtual std::uint32_t error() const=0;
};
class ControlPipe {
public:
    virtual ~ControlPipe()=default;
    virtual bool read_descriptor(std::uint16_t value,std::uint16_t index,std::uint16_t length,
                                 std::vector<std::uint8_t>& bytes)=0;
    // Adopt an already active host configuration; unsupported changes must fail.
    virtual bool use_configuration(std::uint8_t configuration)=0;
    virtual bool use_interface(std::uint8_t interface_number,std::uint8_t alternate)=0;
    virtual bool read_report(std::uint8_t,std::vector<std::uint8_t>&) { return false; }
    virtual bool set_report(std::uint8_t,std::uint8_t,std::uint8_t,std::span<const std::uint8_t>) { return false; }
    virtual std::uint32_t error() const=0;
};
class Host {
public:
    virtual ~Host()=default;
    virtual bool scan(std::vector<Device>& devices)=0;
    virtual std::unique_ptr<ControlPipe> open_control(const Device&) { return {}; }
    virtual std::unique_ptr<InputPipe> open_input(const Device&,const Endpoint&) { return {}; }
    virtual bool location(const Device&,std::array<std::uint8_t,7>&) { return false; }
    virtual std::uint32_t error() const=0;
};
std::unique_ptr<Host> make_host();
}
