#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <array>
#include <nt/image_upload.hpp>
#include <nt/native_scanout.hpp>

namespace nt::graphics {
struct Sprite {
    std::uint32_t left, top, right, bottom, base, pitch, color, mask;
    std::uint32_t texture_base=0, texture_pitch=0, texture_width=0, texture_height=0;
    std::uint32_t flags=0, clamp_u=0, clamp_v=0, texa=0;
    std::int32_t x0=0,y0=0,dx=0,dy=0,u0=0,v0=0,du=0,dv=0;
    std::uint32_t min_u=0,max_u=0,min_v=0,max_v=0;
    std::uint32_t test=0,blend=0;
    std::uint32_t depth_base=0,depth=0;
};
inline constexpr std::uint32_t triangle_flag=1U<<14, shaded_triangle_flag=1U<<15;
inline constexpr std::uint32_t perspective_triangle_flag=1U<<16;
inline constexpr std::uint32_t depth_test_flag=1U<<17, depth_write_flag=1U<<18;
inline constexpr std::uint32_t depth_24_flag=1U<<19, coverage_flag=1U<<20, alpha_blend_flag=1U<<21;
inline constexpr std::uint32_t line_flag=1U<<24;
inline constexpr std::uint32_t indexed8_flag=1U<<25;
inline constexpr std::uint32_t fog_flag=1U<<26;
struct RasterVertex {
    std::int32_t x=0,y=0;
    std::uint32_t color=0;
    std::uint32_t u=0,v=0;
    float s=0,t=0,q=1;
    std::uint32_t z=0,fog=0;
};
struct SpriteCommand {
    Sprite draw;
    std::array<std::uint32_t,256> palette{};
    std::array<RasterVertex,3> triangle{};
    std::uint32_t fog_color=0,fog=0;
};
struct RenderStatistics {
    std::uint64_t submissions=0,sprites=0,upload_bytes=0,readback_bytes=0;
    std::uint64_t triangles=0,texture_snapshot_bytes=0,direct_texture_draws=0;
    std::uint64_t wait_ns=0,upload_ns=0,write_ns=0,draw_ns=0,texture_decode_ns=0;
    std::uint64_t surface_import_ns=0,surface_export_ns=0;
    std::uint64_t surface_conflicts=0,texture_readbacks=0,palette_readbacks=0;
    std::array<std::uint32_t,8> last_surface_conflict{}; // base, pitch, height, depth for each view.
    std::uint64_t uploads=0,feedback_barriers=0,gpu_ns=0;
    std::uint64_t draw_calls=0,batched_primitives=0;
    bool gpu_timing_supported=false;
    bool ordered_fragment_access=false;
    bool llvm_texture_kernels=false;
    std::uint64_t texture_kernel_dispatches=0;
    std::uint64_t texture_kernel_verified=0;
};
// Native Vulkan storage and ordered batches. Native window is an HWND on Windows.
enum class RenderPath { native_images, reference_compute };
class VulkanRenderer {
public:
    explicit VulkanRenderer(void* native_window = nullptr, bool validation = false, RenderPath path = RenderPath::native_images);
    ~VulkanRenderer();
    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;
    void synchronize();
    // Completion tokens refer to actual queue fences, never assumed readiness.
    std::uint64_t enqueue_finish();
    std::uint64_t completed_submission();
    void sprite(std::span<std::uint32_t> memory, const Sprite& draw, std::span<const std::uint32_t> palette = {});
    void sprites(std::span<std::uint32_t> memory, std::span<const SpriteCommand> commands);
    // Resident clients exclusively own this renderer's working storage.
    static void validate(std::span<const SpriteCommand> commands);
    void upload(std::span<const std::uint32_t> memory);
    void draw(std::span<const SpriteCommand> commands);
    void write(std::span<const PixelWrite> writes);
    void read(std::span<std::uint32_t> memory);
    void read_words(std::span<const unsigned> addresses,std::span<std::uint32_t> values);
    const RenderStatistics& statistics() const;
    // Diagnostic readback and exact comparison. Disabled during normal play.
    void verify_texture_kernels(bool enable);
    // GPU display resolve and direct GPU-to-swapchain transfer. Only a changed
    // image flag is read by the host; scanout_readback is an explicit diagnostic.
    bool present_display(DisplayPlan plan);
    // The host resized its window. Retain rendering resources and recreate
    // only presentation objects after their queued work has retired.
    void refresh_surface();
    Scanout scanout_readback(DisplayPlan plan);
    void present(std::span<const std::uint32_t> rgba, unsigned width, unsigned height);
    const std::string& device_name() const;
    unsigned validation_errors() const;
    std::string validation_message() const;
private:
    void present_buffer(bool resident,unsigned width,unsigned height);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
