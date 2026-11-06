#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <array>
#include <algorithm>
#include <nt/image_upload.hpp>
#include <nt/native_scanout.hpp>
#include <nt/vector_draw.hpp>

namespace nt::graphics {
// Per-primitive bounds; std::minmax over an initializer list is a runtime-dispatched call.
struct VertexRange { int low,high; };
inline VertexRange vertex_range(int a,int b,int c) { return {std::min(a,std::min(b,c)),std::max(a,std::max(b,c))}; }
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
// A PSMCT16 or PSMCT16S texture, read from memory by the draw shader.
inline constexpr std::uint32_t texture_16_flag=1U<<22, texture_16s_flag=1U<<23;
inline constexpr std::uint32_t line_flag=1U<<24;
inline constexpr std::uint32_t indexed8_flag=1U<<25;
inline constexpr std::uint32_t fog_flag=1U<<26;
inline constexpr std::uint32_t color_16_flag=1U<<27, color_16s_flag=1U<<28;
// Color written through a Z32/Z24 frame format: depth page layout (block ^ 24).
inline constexpr std::uint32_t color_z_flag=1U<<29;
// A Z32/Z24 texture: sampled as CT32/CT24 in the depth page layout.
inline constexpr std::uint32_t texture_z_flag=1U<<30;
// PSMT8H/PSMT4HL/PSMT4HH: the palette index is in the top bits of a CT32-layout
// word (bits 24-31, 24-27 or 28-31). With texture_high_flag, bit 30 selects
// bits 28-31 of a 4-bit index; texture_z_flag applies only to direct textures.
inline constexpr std::uint32_t texture_high_flag=1U<<31, texture_high_nibble_flag=1U<<30;
inline unsigned texture_high_index(std::uint32_t flags,std::uint32_t word) {
    return (flags&indexed8_flag) ? word>>24 : (word>>((flags&texture_high_nibble_flag) ? 28 : 24))&15U;
}
struct RasterVertex {
    std::int32_t x=0,y=0;
    std::uint32_t color=0;
    std::uint32_t u=0,v=0;
    float s=0,t=0,q=1;
    std::uint32_t z=0,fog=0;
};
// TEX1 level-of-detail sampling. Level 0 is described by Sprite; levels 1-6
// use the same format, palette and wrap mode at their MIPTBP base and width.
struct TextureLevels {
    std::uint32_t control=0; // texture_lod_* fields; zero selects Sprite filtering only.
    float q=1; // Sprite Q. Triangles interpolate their vertex Q per pixel.
    std::array<std::uint32_t,6> base{},pitch{};
};
inline constexpr std::uint32_t texture_lod_enabled=1, texture_lod_minify_linear=1U<<4;
inline constexpr std::uint32_t texture_lod_constant=1U<<7;
inline constexpr unsigned texture_lod_levels(std::uint32_t control) { return (control>>1)&7; }
inline constexpr unsigned texture_lod_mip_mode(std::uint32_t control) { return (control>>5)&3; } // 0 none, 1 nearest, 2 linear.
// A planned primitive before palette snapshot and texture-level planning.
struct PlannedPrimitive {
    Sprite draw;
    std::array<RasterVertex,3> triangle{};
};
struct SpriteCommand {
    Sprite draw;
    std::array<std::uint32_t,256> palette{};
    std::array<RasterVertex,3> triangle{};
    std::uint32_t fog_color=0,fog=0;
    TextureLevels levels{};
    // Nonzero: a 32-bit palette held on the GPU (VulkanRenderer::load_palette)
    // replaces `palette`; palette_bank is the CSA bank of that palette.
    std::uint32_t palette_slot=0,palette_bank=0;
    // `palette` is a snapshot of a GPU-resident palette's entries: keep decoding
    // in the draw shader, as that palette's other draws do.
    bool shader_palette=false;
};
struct RenderStatistics {
    std::uint64_t submissions=0,sprites=0,upload_bytes=0,readback_bytes=0;
    std::uint64_t triangles=0,texture_snapshot_bytes=0,direct_texture_draws=0;
    std::uint64_t texture_misses=0,texture_evictions=0;
    std::uint64_t surface_lookup_ns=0,texture_lookup_ns=0,target_bind_ns=0,draw_record_ns=0,invalidate_ns=0;
    std::uint64_t wait_ns=0,upload_ns=0,write_ns=0,draw_ns=0,texture_decode_ns=0;
    std::uint64_t surface_import_ns=0,surface_export_ns=0;
    std::uint64_t surface_conflicts=0,surface_gpu_imports=0,texture_readbacks=0,palette_readbacks=0;
    std::uint64_t gpu_palette_loads=0,gpu_indexed_decodes=0,shader_indexed_draws=0;
    std::uint64_t texture_recycles=0,surface_palette_loads=0;
    std::uint64_t surface_evictions=0;
    std::array<std::uint32_t,8> last_surface_conflict{}; // base, pitch, height, depth for each view.
    std::uint64_t uploads=0,feedback_barriers=0,gpu_ns=0;
    std::uint64_t vector_gpu_ns=0; // vector batch compute, submitted before a slot's draws
    std::uint64_t draw_calls=0,batched_primitives=0,render_passes=0,ordering_barriers=0;
    bool gpu_timing_supported=false;
    bool ordered_fragment_access=false;
    bool llvm_texture_kernels=false;
    std::uint64_t texture_kernel_dispatches=0;
    std::uint64_t texture_kernel_verified=0;
    std::uint64_t vector_launches=0,vector_resident_hits=0,vector_resident_uploads=0;
    std::uint64_t vector_resident_upload_bytes=0,vector_staged_bytes=0,vector_failures=0;
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
    // Data-independent draw state; GPU assembly validates vertex-dependent bounds.
    static void validate_target(const Sprite& draw);
    // Validates one draw's fields; the palette contents need no validation.
    static void validate(const Sprite& draw,const std::array<RasterVertex,3>& triangle,
        std::uint32_t fog_color,std::uint32_t fog,const TextureLevels& levels);
    void upload(std::span<const std::uint32_t> memory);
    void draw(std::span<const SpriteCommand> commands);
    // Precompiled element kernels only; no runtime instruction translation.
    // GPU primitive assembly for PACKED triangle batches. False when the device
    // lacks ordered fragment access or the renderer is the reference path.
    bool enable_vector_drawing();
    unsigned load_vector_kernel(std::span<const std::uint32_t> spirv,unsigned unit);
    // command carries the batch's target state, palette snapshot and texture
    // levels; its bounds are the conservative scissor rectangle.
    void draw_vectors(const VectorLaunch& launch,const VectorAssembly& assembly,const SpriteCommand& command);
    void write(std::span<const PixelWrite> writes);
    void write(const ImageRect& rect);
    // DIMX matrix for later draws to 16-bit targets; zero leaves colors unchanged.
    void set_dither(std::uint64_t matrix);
    void read(std::span<std::uint32_t> memory);
    // As read(), current at least for the words [start, start+length).
    void read(std::span<std::uint32_t> memory,unsigned start,unsigned length);
    void read_words(std::span<const unsigned> addresses,std::span<std::uint32_t> values);
    // Native renderer: a slot for palette entries first.. whose words lie in rendered
    // memory. Only those entries are defined; 0 means read the words instead.
    std::uint32_t load_palette(std::span<const unsigned> addresses,unsigned first);
    // A slot combining sixteen-entry banks: each from its slot, or from `current` when 0.
    std::uint32_t merge_palette(const std::array<std::uint32_t,16>& banks,const std::array<std::uint32_t,256>& current);
    // No later draw selects this slot, so its contents need not outlive their memory.
    void release_palette(std::uint32_t slot);
    void read_palette(std::uint32_t slot,std::span<std::uint32_t,256> words);
    const RenderStatistics& statistics() const;
    // Diagnostic readback and exact comparison. Disabled during normal play.
    void verify_texture_kernels(bool enable);
    // Per-command host timing is opt-in; counters remain available without it.
    void profile_commands(bool enable);
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
