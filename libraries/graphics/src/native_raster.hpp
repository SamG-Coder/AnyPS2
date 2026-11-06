#pragma once
#include <nt/vulkan_renderer.hpp>
#include <vulkan/vulkan.h>
#include <memory>
namespace nt::graphics {
// Image-backed hardware rasterization. Console layouts exist only at import/export.
class NativeRaster {
public:
    NativeRaster(VkPhysicalDevice physical,VkDevice device,VkQueue queue,unsigned family,bool ordered_fragments,unsigned resolution_scale=1);
    ~NativeRaster();
    void upload(std::span<const std::uint32_t> words);
    void write(std::span<const PixelWrite> writes);
    // Whole 32-bit pixels of a transfer rectangle: one swizzle pass on the CPU
    // and one GPU pass into rendered surfaces, without per-pixel records.
    void write(const ImageRect& rect);
    void set_dither(std::uint64_t matrix);
    void draw(std::span<const SpriteCommand> commands);
    // Vector batches need ordered fragment access; returns false without it.
    bool enable_vectors();
    unsigned load_vector_kernel(std::span<const std::uint32_t> spirv,unsigned unit);
    void draw_vectors(const VectorLaunch&,const VectorAssembly&,const SpriteCommand&);
    // `words` receives all of memory; only [start, start+length) is
    // guaranteed current with rendered surfaces.
    void read(std::span<std::uint32_t> words,unsigned start=0,unsigned length=1024*1024);
    void read_words(std::span<const unsigned> addresses,std::span<std::uint32_t> values);
    // A slot for palette entries first.. whose words lie in rendered memory. Only
    // those entries are defined. Returns 0 when the caller must read the words
    // instead. read_palette returns a slot's words.
    std::uint32_t load_palette(std::span<const unsigned> addresses,unsigned first);
    std::uint32_t merge_palette(const std::array<std::uint32_t,16>& banks,const std::array<std::uint32_t,256>& current);
    void release_palette(std::uint32_t slot);
    std::vector<std::uint32_t> read_surface_samples(unsigned base,unsigned pitch,unsigned format,unsigned& width,unsigned& height);
    void read_palette(std::uint32_t slot,std::span<std::uint32_t,256> words);
    void finish();
    // Completion tokens refer to actual queue fences, never assumed readiness.
    std::uint64_t enqueue_finish();
    std::uint64_t completed_submission();
    void wait_submission(std::uint64_t token);
    bool changed(const DisplayPlan& plan);
    void release_presentation();
    std::uint64_t present(const DisplayPlan& plan,VkImage image,VkFormat format,VkSemaphore acquired,VkSemaphore rendered);
    Scanout capture(const DisplayPlan& plan);
    const RenderStatistics& statistics() const;
    void verify_texture_kernels(bool enable);
    // Per-command host timing is opt-in; counters remain available without it.
    void profile_commands(bool enable);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
