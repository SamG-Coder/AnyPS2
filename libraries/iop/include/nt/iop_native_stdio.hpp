#pragma once
#include <nt/iop_native_format.hpp>
#include <algorithm>
#include <bit>
#include <charconv>
#include <functional>
#include <string>
#include <string_view>
#include <stdexcept>

namespace nt::iop {
class NativeStdioServices final : public NativeServices {
public:
    using Sink=std::function<bool(std::string_view)>;
    explicit NativeStdioServices(Sink sink) : sink_(std::move(sink)) {
        if (!sink_) throw std::invalid_argument("Native stdio requires an output sink");
    }
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t> ram) override {
        const bool kernel=request.kind==NativeServiceKind::sysmem && request.selector==14;
        if (!kernel && (request.kind!=NativeServiceKind::stdio || request.selector!=4)) return {};
        // The configured host sink replaces diagnostic output. A null Kprintf
        // format is a callback-specific control request, not an empty string.
        if (kernel && !request.arguments[0]) return {};
        std::string output;
        const auto formatted=format_native(request.arguments[0],ram,NativeFormatArguments(request,ram,1),output);
        if (formatted.status!=NativeServiceStatus::returned) return formatted;
        // No output is emitted until the entire format and argument set validate.
        if (!sink_(output)) return {NativeServiceStatus::returned,0xffffffff};
        return {NativeServiceStatus::returned,static_cast<std::uint32_t>(output.size())};
    }
private:
    Sink sink_;
};
}
