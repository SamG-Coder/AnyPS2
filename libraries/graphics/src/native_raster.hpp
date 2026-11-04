#pragma once
#include <nt/vulkan_renderer.hpp>
#include <vulkan/vulkan.h>
#include <memory>
namespace nt::graphics {
// Image-backed hardware rasterization. Console layouts exist only at import/export.
class NativeRaster {
public:
    NativeRaster(VkPhysicalDevice physical,VkDevice device,VkQueue queue,unsigned family,bool ordered_fragments);
    ~NativeRaster();
    void upload(std::span<const std::uint32_t> words);
    void write(std::span<const PixelWrite> writes);
    void draw(std::span<const SpriteCommand> commands);
    // Vector batches need ordered fragment access; returns false without it.
    bool enable_vectors();
    unsigned load_vector_kernel(std::span<const std::uint32_t> spirv,unsigned unit);
    void draw_vectors(const VectorLaunch&,const VectorAssembly&,const Sprite&);
    void read(std::span<std::uint32_t> words);
    void read_words(std::span<const unsigned> addresses,std::span<std::uint32_t> values);
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
