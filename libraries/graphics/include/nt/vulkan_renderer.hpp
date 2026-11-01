#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <array>

namespace nt::graphics {
struct Sprite {
    std::uint32_t left, top, right, bottom, base, pitch, color, mask;
    std::uint32_t texture_base=0, texture_pitch=0, texture_width=0, texture_height=0;
    std::uint32_t flags=0, clamp_u=0, clamp_v=0, texa=0;
    std::int32_t x0=0,y0=0,dx=0,dy=0,u0=0,v0=0,du=0,dv=0;
    std::uint32_t min_u=0,max_u=0,min_v=0,max_v=0;
    std::uint32_t test=0,blend=0;
};
struct SpriteCommand {
    Sprite draw;
    std::array<std::uint32_t,16> palette{};
};
struct RenderStatistics {
    std::uint64_t submissions=0,sprites=0,upload_bytes=0,readback_bytes=0;
};
// Synchronous reference backend. Native window is an HWND on Windows.
class VulkanRenderer {
public:
    explicit VulkanRenderer(void* native_window = nullptr, bool validation = false);
    ~VulkanRenderer();
    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;
    void synchronize();
    void sprite(std::span<std::uint32_t> memory, const Sprite& draw, std::span<const std::uint32_t> palette = {});
    void sprites(std::span<std::uint32_t> memory, std::span<const SpriteCommand> commands);
    const RenderStatistics& statistics() const;
    void present(std::span<const std::uint32_t> rgba, unsigned width, unsigned height);
    const std::string& device_name() const;
    unsigned validation_errors() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
