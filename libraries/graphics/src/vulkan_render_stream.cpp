#include <nt/vulkan_render_stream.hpp>
#include <array>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace nt::graphics {
namespace {
void checked(VkResult result,const char* operation) {
    if (result!=VK_SUCCESS) throw std::runtime_error(std::string(operation)+": Vulkan result "+std::to_string(result));
}
template<class T> std::uint64_t identity(T handle) {
    if constexpr(std::is_pointer_v<T>) return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(handle));
    else return static_cast<std::uint64_t>(handle);
}
RenderStreamRange allocation_range(const RenderBufferView& view) {
    if (view.offset>std::numeric_limits<VkDeviceSize>::max()-view.memory_offset)
        throw std::invalid_argument("Render stream allocation offset overflows");
    return {identity(view.memory),view.memory_offset+view.offset,view.bytes};
}
void validate_view(const RenderBufferView& view,const VkPhysicalDeviceLimits& limits,VkBufferUsageFlags required) {
    if (!view.buffer || !view.memory || !view.bytes || !view.buffer_bytes ||
        view.offset>view.buffer_bytes || view.bytes>view.buffer_bytes-view.offset ||
        view.offset%4 || view.bytes%4 || (view.usage&required)!=required ||
        view.offset%limits.minStorageBufferOffsetAlignment || view.bytes>limits.maxStorageBufferRange)
        throw std::invalid_argument("Invalid render stream Vulkan buffer view, usage or descriptor range");
    (void)allocation_range(view);
}
VkBufferMemoryBarrier barrier(VkBuffer buffer,VkDeviceSize offset,VkDeviceSize bytes,
    VkAccessFlags source,VkAccessFlags destination) {
    VkBufferMemoryBarrier result{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    result.srcAccessMask=source; result.dstAccessMask=destination;
    result.srcQueueFamilyIndex=result.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    result.buffer=buffer; result.offset=offset; result.size=bytes;
    return result;
}
}
struct VulkanRenderKernel::Impl {
    VkDevice device{}; VkPhysicalDevice physical{};
    VkPhysicalDeviceProperties properties{};
    RenderStreamLayout contract;
    VkDescriptorSetLayout descriptors{}; VkPipelineLayout layout{}; VkPipeline pipeline{};
    ~Impl() {
        if (pipeline) vkDestroyPipeline(device,pipeline,nullptr);
        if (layout) vkDestroyPipelineLayout(device,layout,nullptr);
        if (descriptors) vkDestroyDescriptorSetLayout(device,descriptors,nullptr);
    }
};
VulkanRenderKernel::VulkanRenderKernel(VkPhysicalDevice physical,VkDevice device,
    std::span<const std::uint32_t> spirv,RenderStreamLayout contract):impl_(std::make_unique<Impl>()) {
    validate(contract);
    if (!physical || !device || spirv.size()<5 || spirv.size()>4*1024*1024 || spirv[0]!=0x07230203 ||
        !spirv[3] || spirv[4]!=0)
        throw std::invalid_argument("Invalid render stream device or SPIR-V envelope");
    auto& k=*impl_; k.device=device; k.physical=physical; k.contract=std::move(contract);
    vkGetPhysicalDeviceProperties(physical,&k.properties);
    const auto& limits=k.properties.limits;
    if (k.properties.apiVersion<VK_API_VERSION_1_3 || limits.maxComputeWorkGroupInvocations<64 ||
        limits.maxComputeWorkGroupSize[0]<64 || limits.maxPerStageDescriptorStorageBuffers<3 ||
        limits.maxDescriptorSetStorageBuffers<3 || !limits.minStorageBufferOffsetAlignment)
        throw std::invalid_argument("Device cannot run the Vulkan 1.3 render stream interface");
    std::array<VkDescriptorSetLayoutBinding,3> bindings{};
    for (unsigned n=0;n<bindings.size();++n)
        bindings[n]={n,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    VkDescriptorSetLayoutCreateInfo descriptor_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    descriptor_info.bindingCount=static_cast<std::uint32_t>(bindings.size()); descriptor_info.pBindings=bindings.data();
    checked(vkCreateDescriptorSetLayout(device,&descriptor_info,nullptr,&k.descriptors),"Create stream descriptors");
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount=1; layout_info.pSetLayouts=&k.descriptors;
    checked(vkCreatePipelineLayout(device,&layout_info,nullptr,&k.layout),"Create stream pipeline layout");
    VkShaderModule module{};
    VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    module_info.codeSize=spirv.size_bytes(); module_info.pCode=spirv.data();
    checked(vkCreateShaderModule(device,&module_info,nullptr,&module),"Create validated stream shader");
    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.layout=k.layout;
    info.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,module,"main",nullptr};
    const auto result=vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&info,nullptr,&k.pipeline);
    vkDestroyShaderModule(device,module,nullptr);
    checked(result,"Create stream compute pipeline");
}
VulkanRenderKernel::~VulkanRenderKernel()=default;
struct VulkanRenderStream::Impl {
    std::shared_ptr<const VulkanRenderKernel> kernel;
    RenderBufferView source,output;
    VkDevice device{}; VkBuffer parameters{}; VkDeviceMemory memory{};
    VkDeviceSize parameter_bytes=0;
    VkDescriptorPool pool{}; VkDescriptorSet descriptors{};
    ~Impl() {
        // The submission owner must have completed all work before destruction.
        if (pool) vkDestroyDescriptorPool(device,pool,nullptr);
        if (parameters) vkDestroyBuffer(device,parameters,nullptr);
        if (memory) vkFreeMemory(device,memory,nullptr);
    }
};
VulkanRenderStream::VulkanRenderStream(std::shared_ptr<const VulkanRenderKernel> kernel,
    RenderBufferView source,RenderBufferView output):impl_(std::make_unique<Impl>()) {
    if (!kernel) throw std::invalid_argument("Render stream needs a compiled kernel");
    const auto& k=*kernel->impl_;
    validate_view(source,k.properties.limits,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    validate_view(output,k.properties.limits,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    if (source.buffer==output.buffer && (source.memory!=output.memory || source.memory_offset!=output.memory_offset))
        throw std::invalid_argument("Inconsistent allocation identity for the same Vulkan buffer");
    const auto a=allocation_range(source),b=allocation_range(output);
    // Validate allocation arithmetic and aliasing before allocating Vulkan objects.
    std::vector<std::uint32_t> minimum(k.contract.input_strides.size()+1); minimum[0]=1;
    (void)plan_render_stream(k.contract,minimum,a,b);
    auto& s=*impl_; s.device=k.device; s.kernel=std::move(kernel); s.source=source; s.output=output;
    s.parameter_bytes=minimum.size()*sizeof(std::uint32_t);
    if (s.parameter_bytes>k.properties.limits.maxStorageBufferRange)
        throw std::invalid_argument("Render stream parameter descriptor exceeds device limits");
    VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer.size=s.parameter_bytes; buffer.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    checked(vkCreateBuffer(s.device,&buffer,nullptr,&s.parameters),"Create stream parameter buffer");
    VkMemoryRequirements requirements{}; vkGetBufferMemoryRequirements(s.device,s.parameters,&requirements);
    VkPhysicalDeviceMemoryProperties memory_properties{}; vkGetPhysicalDeviceMemoryProperties(k.physical,&memory_properties);
    unsigned memory_type=memory_properties.memoryTypeCount;
    for (unsigned n=0;n<memory_properties.memoryTypeCount;++n)
        if ((requirements.memoryTypeBits&(1U<<n)) &&
            (memory_properties.memoryTypes[n].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { memory_type=n; break; }
    if (memory_type==memory_properties.memoryTypeCount)
        throw std::runtime_error("No device-local memory for render stream parameters");
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize=requirements.size; allocation.memoryTypeIndex=memory_type;
    checked(vkAllocateMemory(s.device,&allocation,nullptr,&s.memory),"Allocate stream parameters");
    checked(vkBindBufferMemory(s.device,s.parameters,s.memory,0),"Bind stream parameters");
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,3};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets=1; pool.poolSizeCount=1; pool.pPoolSizes=&pool_size;
    checked(vkCreateDescriptorPool(s.device,&pool,nullptr,&s.pool),"Create stream descriptor pool");
    VkDescriptorSetAllocateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    set.descriptorPool=s.pool; set.descriptorSetCount=1; set.pSetLayouts=&k.descriptors;
    checked(vkAllocateDescriptorSets(s.device,&set,&s.descriptors),"Allocate stream descriptor set");
    const std::array<VkDescriptorBufferInfo,3> buffers{{
        {source.buffer,source.offset,source.bytes},{output.buffer,output.offset,output.bytes},
        {s.parameters,0,s.parameter_bytes}}};
    std::array<VkWriteDescriptorSet,3> writes{};
    for (unsigned n=0;n<writes.size();++n) {
        writes[n]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[n].dstSet=s.descriptors; writes[n].dstBinding=n; writes[n].descriptorCount=1;
        writes[n].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[n].pBufferInfo=&buffers[n];
    }
    vkUpdateDescriptorSets(s.device,static_cast<std::uint32_t>(writes.size()),writes.data(),0,nullptr);
}
VulkanRenderStream::~VulkanRenderStream()=default;
RecordedVertices VulkanRenderStream::record(VkCommandBuffer command,std::span<const std::uint32_t> parameters,
    StreamSourceProducer producer) {
    auto& s=*impl_; const auto& k=*s.kernel->impl_;
    // No commands are recorded until the complete batch has been accepted.
    const auto plan=plan_render_stream(k.contract,parameters,allocation_range(s.source),allocation_range(s.output));
    if (!command || plan.groups>k.properties.limits.maxComputeWorkGroupCount[0])
        throw std::invalid_argument("Invalid stream command buffer or dispatch extent");
    if (producer!=StreamSourceProducer::visible && producer!=StreamSourceProducer::transfer &&
        producer!=StreamSourceProducer::compute)
        throw std::invalid_argument("Unknown render stream source dependency");
    const std::array reuse{
        barrier(s.parameters,0,s.parameter_bytes,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_TRANSFER_WRITE_BIT),
        barrier(s.output.buffer,s.output.offset,s.output.bytes,VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_WRITE_BIT)};
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_VERTEX_INPUT_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,
        static_cast<std::uint32_t>(reuse.size()),reuse.data(),0,nullptr);
    vkCmdUpdateBuffer(command,s.parameters,0,s.parameter_bytes,parameters.data());
    std::array<VkBufferMemoryBarrier,2> ready{};
    ready[0]=barrier(s.parameters,0,s.parameter_bytes,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
    unsigned barriers=1; VkPipelineStageFlags stages=VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (producer!=StreamSourceProducer::visible) {
        const bool transfer=producer==StreamSourceProducer::transfer;
        ready[1]=barrier(s.source.buffer,s.source.offset,s.source.bytes,
            transfer ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
        stages|=transfer ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        barriers=2;
    }
    vkCmdPipelineBarrier(command,stages,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,barriers,ready.data(),0,nullptr);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,k.pipeline);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,k.layout,0,1,&s.descriptors,0,nullptr);
    vkCmdDispatch(command,plan.groups,1,1);
    const auto vertices=barrier(s.output.buffer,s.output.offset,plan.output_bytes,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT);
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
        0,0,nullptr,1,&vertices,0,nullptr);
    return {s.output.buffer,s.output.offset,plan.items};
}
}
