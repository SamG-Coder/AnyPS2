#pragma once
#include <nt/vector_draw.hpp>
#include <vulkan/vulkan.h>
#include <memory>
#include <span>

namespace nt::graphics {
// Device memory, compiled element kernels and primitive assembly for GPU vector
// batches (vector_draw.hpp). Work is recorded into the owner's command buffer for
// one frame slot. The owner must have completed a slot's previous submission
// before collect() and reuse, and must have drained all work before
// clear_resident(). After a slot's vector work and before its draws or host
// reads, the owner records one barrier from compute and transfer writes to
// vertex, fragment and host reads.
class NativeVectors {
public:
    static constexpr unsigned record_words=80;
    struct Statistics {
        std::uint64_t launches=0,resident_hits=0,resident_uploads=0;
        std::uint64_t resident_upload_bytes=0,staged_bytes=0,failures=0;
    };
    NativeVectors(VkPhysicalDevice physical,VkDevice device,VkBuffer records,VkDeviceSize record_bytes,unsigned slots);
    ~NativeVectors();
    NativeVectors(const NativeVectors&)=delete;
    NativeVectors& operator=(const NativeVectors&)=delete;
    unsigned load_kernel(std::span<const std::uint32_t> spirv,unsigned unit);
    // Throws for a launch no slot could hold or that names an unknown kernel.
    void validate(const VectorLaunch& launch) const;
    enum class Space { fits, slot_full, resident_full };
    Space space(unsigned slot,const VectorLaunch& launch) const;
    // Records the launch's memory writes, element kernel and primitive assembly.
    // Assembly writes records [first_record, first_record+primitives) of the
    // owner's record buffer from the 68-word primitive template.
    void record(VkCommandBuffer command,unsigned slot,const VectorLaunch& launch,const VectorAssembly& assembly,
        std::span<const std::uint32_t,record_words> primitive,unsigned first_record,unsigned primitives);
    // Kernel launches record their element work into command; host-record
    // launches need no command buffer (pass VK_NULL_HANDLE). Every launch only
    // reserves its assembly. record_upload, recorded after all of a slot's
    // record() calls (it may append to the same command buffer), uploads the
    // slot's staging and assembles every batch in one dispatch, followed by the
    // barrier to vertex, fragment and host reads.
    bool pending(unsigned slot) const;
    void record_upload(VkCommandBuffer command,unsigned slot);
    // After the slot's fence: adds element failures to the statistics, resets the slot.
    void collect(unsigned slot);
    void clear_resident();
    const Statistics& statistics() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
