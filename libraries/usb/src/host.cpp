#include <nt/usb.hpp>
#include <algorithm>
#include <cstring>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <initguid.h>
#include <usbiodef.h>
#include <usbioctl.h>
#endif

namespace nt::usb {
bool valid_descriptors(std::span<const std::uint8_t> bytes) {
    if (bytes.size()<18 || bytes[0]!=18 || bytes[1]!=1 || !bytes[17]) return false;
    unsigned configurations=0;
    for (std::size_t at=18;at<bytes.size();) {
        if (bytes.size()-at<9 || bytes[at]!=9 || bytes[at+1]!=2) return false;
        const auto size=unsigned(bytes[at+2])|(unsigned(bytes[at+3])<<8);
        if (size<9 || size>bytes.size()-at) return false;
        const auto end=at+size;
        for (;at<end;) {
            if (end-at<2 || bytes[at]<2 || bytes[at]>end-at) return false;
            at+=bytes[at];
        }
        ++configurations;
    }
    return configurations==bytes[17];
}
namespace {
#ifdef _WIN32
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    ~Handle() { if (value!=INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct DeviceSet {
    HDEVINFO value=INVALID_HANDLE_VALUE;
    ~DeviceSet() { if (value!=INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(value); }
};
class WindowsHost final : public Host {
public:
    bool scan(std::vector<Device>& devices) override {
        error_=0;
        DeviceSet set{SetupDiGetClassDevsW(&GUID_DEVINTERFACE_USB_HUB,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE)};
        if (set.value==INVALID_HANDLE_VALUE) return fail(GetLastError());
        std::vector<Device> snapshot;
        std::size_t total_bytes=0;
        for (DWORD index=0;;++index) {
            SP_DEVICE_INTERFACE_DATA interface{}; interface.cbSize=sizeof(interface);
            if (!SetupDiEnumDeviceInterfaces(set.value,nullptr,&GUID_DEVINTERFACE_USB_HUB,index,&interface)) {
                const auto error=GetLastError();
                if (error!=ERROR_NO_MORE_ITEMS) return fail(error);
                break;
            }
            if (index>=256) return fail(ERROR_TOO_MANY_NAMES);
            DWORD needed=0;
            SetupDiGetDeviceInterfaceDetailW(set.value,&interface,nullptr,0,&needed,nullptr);
            if (GetLastError()!=ERROR_INSUFFICIENT_BUFFER || needed<sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) || needed>65536)
                return fail(ERROR_INVALID_DATA);
            std::vector<std::uint8_t> storage(needed);
            auto* detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
            detail->cbSize=sizeof(*detail);
            if (!SetupDiGetDeviceInterfaceDetailW(set.value,&interface,detail,needed,nullptr,nullptr)) return fail(GetLastError());
            Handle hub{CreateFileW(detail->DevicePath,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,
                                   nullptr,OPEN_EXISTING,0,nullptr)};
            if (hub.value==INVALID_HANDLE_VALUE) return fail(GetLastError());
            USB_NODE_INFORMATION node{}; node.NodeType=UsbHub;
            DWORD received=0;
            if (!DeviceIoControl(hub.value,IOCTL_USB_GET_NODE_INFORMATION,&node,sizeof(node),&node,sizeof(node),&received,nullptr))
                return fail(GetLastError());
            if (received<sizeof(node)) return fail(ERROR_INVALID_DATA);
            for (unsigned port=1;port<=node.u.HubInformation.HubDescriptor.bNumberOfPorts;++port) {
                // Include room for all USB endpoint addresses in the returned pipe array.
                alignas(USB_NODE_CONNECTION_INFORMATION_EX) std::uint8_t connection_storage[
                    sizeof(USB_NODE_CONNECTION_INFORMATION_EX)+32*sizeof(USB_PIPE_INFO)]{};
                auto* connection=reinterpret_cast<USB_NODE_CONNECTION_INFORMATION_EX*>(connection_storage);
                connection->ConnectionIndex=port;
                if (!DeviceIoControl(hub.value,IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX,connection,sizeof(connection_storage),
                                     connection,sizeof(connection_storage),&received,nullptr)) return fail(GetLastError());
                if (received<offsetof(USB_NODE_CONNECTION_INFORMATION_EX,PipeList)) return fail(ERROR_INVALID_DATA);
                if (connection->ConnectionStatus!=DeviceConnected) continue;
                Device device; device.key=std::wstring(detail->DevicePath)+L"#"+std::to_wstring(port);
                device.descriptors.resize(sizeof(USB_DEVICE_DESCRIPTOR));
                std::memcpy(device.descriptors.data(),&connection->DeviceDescriptor,sizeof(USB_DEVICE_DESCRIPTOR));
                for (unsigned configuration=0;configuration<connection->DeviceDescriptor.bNumConfigurations;++configuration) {
                    std::vector<std::uint8_t> header,descriptor;
                    if (!configuration_descriptor(hub.value,port,configuration,9,header)) return false;
                    if (header[0]!=9 || header[1]!=2) return fail(ERROR_INVALID_DATA);
                    const auto size=unsigned(header[2])|(unsigned(header[3])<<8);
                    if (size<9 || !configuration_descriptor(hub.value,port,configuration,size,descriptor)) return fail(error_ ? error_ : ERROR_INVALID_DATA);
                    if (!std::equal(header.begin(),header.end(),descriptor.begin())) return fail(ERROR_INVALID_DATA);
                    if (descriptor.size()>0x100000-device.descriptors.size()) return fail(ERROR_NOT_ENOUGH_MEMORY);
                    device.descriptors.insert(device.descriptors.end(),descriptor.begin(),descriptor.end());
                }
                if (!valid_descriptors(device.descriptors)) return fail(ERROR_INVALID_DATA);
                total_bytes+=device.descriptors.size();
                if (total_bytes>16*1024*1024) return fail(ERROR_NOT_ENOUGH_MEMORY);
                snapshot.push_back(std::move(device));
                if (snapshot.size()>1024) return fail(ERROR_TOO_MANY_NAMES);
            }
        }
        std::sort(snapshot.begin(),snapshot.end(),[](const auto& a,const auto& b) { return a.key<b.key; });
        devices=std::move(snapshot);
        return true;
    }
    std::uint32_t error() const override { return error_; }
private:
    bool fail(DWORD error) { error_=error; return false; }
    bool configuration_descriptor(HANDLE hub,unsigned port,unsigned index,unsigned size,std::vector<std::uint8_t>& output) {
        std::vector<std::uint8_t> bytes(sizeof(USB_DESCRIPTOR_REQUEST)+size);
        auto* request=reinterpret_cast<USB_DESCRIPTOR_REQUEST*>(bytes.data());
        request->ConnectionIndex=port;
        request->SetupPacket.bmRequest=0x80; request->SetupPacket.bRequest=6;
        request->SetupPacket.wValue=static_cast<USHORT>(0x200|index);
        request->SetupPacket.wLength=static_cast<USHORT>(size);
        DWORD received=0;
        if (!DeviceIoControl(hub,IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION,request,static_cast<DWORD>(bytes.size()),
                            request,static_cast<DWORD>(bytes.size()),&received,nullptr)) return fail(GetLastError());
        const auto offset=offsetof(USB_DESCRIPTOR_REQUEST,Data);
        if (received<offset+size) return fail(ERROR_INVALID_DATA);
        output.assign(bytes.begin()+offset,bytes.begin()+offset+size);
        return true;
    }
    DWORD error_=0;
};
#else
class WindowsHost final : public Host {
public:
    bool scan(std::vector<Device>&) override { return false; }
    std::uint32_t error() const override { return 50; }
};
#endif
}
std::unique_ptr<Host> make_host() { return std::make_unique<WindowsHost>(); }
}
