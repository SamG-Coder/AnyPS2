#include "render_loop_fixture.hpp"
#include <nt/render_stream_layout.hpp>
#include <nt/vulkan_render_stream.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

namespace {
using namespace nt::graphics;
constexpr unsigned width=128,height=96,pixel_bytes=width*height*4;
constexpr unsigned maximum_items=129,sentinel=0xdeadbeef;
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
void check(VkResult result,const char* message) {
    if (result!=VK_SUCCESS) throw std::runtime_error(std::string(message)+": "+std::to_string(result));
}
std::vector<std::uint32_t> shader(const char* path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    const auto size=file.tellg();
    require(size>=20 && size<16*1024*1024 && size%4==0,"Invalid compiled stream shader file");
    std::vector<std::uint32_t> result(static_cast<std::size_t>(size)/4);
    file.seekg(0); file.read(reinterpret_cast<char*>(result.data()),size);
    require(bool(file),"Cannot read compiled stream shader"); return result;
}
struct Buffer {
    VkBuffer handle{}; VkDeviceMemory memory{}; VkDeviceSize bytes=0; VkBufferUsageFlags usage=0; void* mapping=nullptr;
    RenderBufferView view(VkDeviceSize offset=0,VkDeviceSize extent=0) const {
        return {handle,memory,0,bytes,offset,extent ? extent : bytes-offset,usage};
    }
};
struct Image {
    VkImage handle{}; VkDeviceMemory memory{}; VkImageView view{}; VkFramebuffer framebuffer{};
    VkImageLayout state=VK_IMAGE_LAYOUT_UNDEFINED;
};
struct Device {
    VkInstance instance{}; VkPhysicalDevice physical{}; VkDevice device{}; VkQueue queue{}; unsigned family=0;
    VkPhysicalDeviceProperties properties{};
    VkDebugUtilsMessengerEXT messenger{}; std::atomic<unsigned> errors=0;
    VkCommandPool pool{}; VkFence fence{};
    VkRenderPass pass{}; VkPipelineLayout layout{}; VkPipeline pipeline{};
    std::vector<Buffer> buffers; std::vector<Image> images;
    static VKAPI_ATTR VkBool32 VKAPI_CALL validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* data,void* context) {
        if (severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
            ++static_cast<Device*>(context)->errors; std::cerr<<data->pMessage<<'\n';
        }
        return VK_FALSE;
    }
    void clear() {
        if (!device) return;
        // Test teardown only. The stream recorder itself has no queue wait API.
        vkDeviceWaitIdle(device);
        for (auto image:images) {
            if (image.framebuffer) vkDestroyFramebuffer(device,image.framebuffer,nullptr);
            if (image.view) vkDestroyImageView(device,image.view,nullptr);
            if (image.handle) vkDestroyImage(device,image.handle,nullptr);
            if (image.memory) vkFreeMemory(device,image.memory,nullptr);
        }
        images.clear();
        for (auto buffer:buffers) {
            if (buffer.mapping) vkUnmapMemory(device,buffer.memory);
            if (buffer.handle) vkDestroyBuffer(device,buffer.handle,nullptr);
            if (buffer.memory) vkFreeMemory(device,buffer.memory,nullptr);
        }
        buffers.clear();
        if (pipeline) vkDestroyPipeline(device,std::exchange(pipeline,{}),nullptr);
        if (layout) vkDestroyPipelineLayout(device,std::exchange(layout,{}),nullptr);
        if (pass) vkDestroyRenderPass(device,std::exchange(pass,{}),nullptr);
        if (fence) vkDestroyFence(device,std::exchange(fence,{}),nullptr);
        if (pool) vkDestroyCommandPool(device,std::exchange(pool,{}),nullptr);
    }
    ~Device() {
        clear();
        if (device) vkDestroyDevice(device,nullptr);
        if (messenger) reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT"))(instance,messenger,nullptr);
        if (instance) vkDestroyInstance(instance,nullptr);
    }
    void initialize() {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion=VK_API_VERSION_1_3;
        const char* layer="VK_LAYER_KHRONOS_validation";
        const std::array extensions{VK_EXT_DEBUG_UTILS_EXTENSION_NAME,VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
        const VkValidationFeatureEnableEXT enable=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
        VkValidationFeaturesEXT validation_features{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
        validation_features.enabledValidationFeatureCount=1; validation_features.pEnabledValidationFeatures=&enable;
        VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        debug.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debug.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
        debug.pfnUserCallback=validation; debug.pUserData=this; debug.pNext=&validation_features;
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; info.pApplicationInfo=&app; info.pNext=&debug;
        info.enabledLayerCount=1; info.ppEnabledLayerNames=&layer;
        info.enabledExtensionCount=static_cast<unsigned>(extensions.size()); info.ppEnabledExtensionNames=extensions.data();
        check(vkCreateInstance(&info,nullptr,&instance),"Create headless validation instance");
        debug.pNext=nullptr;
        const auto create=reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkCreateDebugUtilsMessengerEXT"));
        require(create!=nullptr,"Missing Vulkan debug messenger entry");
        check(create(instance,&debug,nullptr,&messenger),"Create validation messenger");
        unsigned count=0; check(vkEnumeratePhysicalDevices(instance,&count,nullptr),"Count Vulkan devices");
        std::vector<VkPhysicalDevice> devices(count);
        check(vkEnumeratePhysicalDevices(instance,&count,devices.data()),"Enumerate Vulkan devices");
        int best=-1;
        for (auto candidate:devices) {
            VkPhysicalDeviceProperties candidate_properties{}; vkGetPhysicalDeviceProperties(candidate,&candidate_properties);
            if (candidate_properties.apiVersion<VK_API_VERSION_1_3) continue;
            unsigned families=0; vkGetPhysicalDeviceQueueFamilyProperties(candidate,&families,nullptr);
            std::vector<VkQueueFamilyProperties> queues(families);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate,&families,queues.data());
            for (unsigned n=0;n<families;++n) if (queues[n].queueCount &&
                (queues[n].queueFlags&(VK_QUEUE_COMPUTE_BIT|VK_QUEUE_GRAPHICS_BIT))==(VK_QUEUE_COMPUTE_BIT|VK_QUEUE_GRAPHICS_BIT)) {
                const int score=candidate_properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 :
                    candidate_properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
                if (score>best) { best=score; physical=candidate; family=n; properties=candidate_properties; }
                break;
            }
        }
        require(physical!=VK_NULL_HANDLE,"No Vulkan 1.3 graphics+compute device; this test must not silently skip");
        std::cout<<"device="<<properties.deviceName<<" device_type="<<properties.deviceType<<'\n';
        VkPhysicalDeviceShaderFloat16Int8Features integers{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};
        VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        features13.pNext=&integers;
        VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; query.pNext=&features13;
        vkGetPhysicalDeviceFeatures2(physical,&query);
        const bool int8=integers.shaderInt8,maintenance4=features13.maintenance4;
        integers={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES}; integers.shaderInt8=int8;
        features13={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        features13.maintenance4=maintenance4; features13.pNext=&integers;
        // Some LLVM resource-name lowerings retain i8 constants in the module.
        require(int8,"This LLVM resource ABI needs a device with shaderInt8");
        const float priority=1;
        VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_info.queueFamilyIndex=family; queue_info.queueCount=1; queue_info.pQueuePriorities=&priority;
        VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; device_info.pNext=&features13;
        device_info.queueCreateInfoCount=1; device_info.pQueueCreateInfos=&queue_info;
        check(vkCreateDevice(physical,&device_info,nullptr,&device),"Create graphics+compute device");
        vkGetDeviceQueue(device,family,0,&queue);
        VkCommandPoolCreateInfo commands{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; commands.queueFamilyIndex=family;
        check(vkCreateCommandPool(device,&commands,nullptr,&pool),"Create command pool");
        VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(vkCreateFence(device,&fence_info,nullptr,&fence),"Create stream test fence");
    }
    VkDeviceMemory allocate(VkMemoryRequirements requirements,VkMemoryPropertyFlags flags) {
        VkPhysicalDeviceMemoryProperties memory{}; vkGetPhysicalDeviceMemoryProperties(physical,&memory);
        unsigned index=memory.memoryTypeCount;
        for (unsigned n=0;n<memory.memoryTypeCount;++n)
            if ((requirements.memoryTypeBits&(1U<<n)) && (memory.memoryTypes[n].propertyFlags&flags)==flags) { index=n; break; }
        require(index<memory.memoryTypeCount,"No compatible stream-test memory type");
        VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        info.allocationSize=requirements.size; info.memoryTypeIndex=index;
        VkDeviceMemory result{}; check(vkAllocateMemory(device,&info,nullptr,&result),"Allocate stream-test memory"); return result;
    }
    Buffer buffer(VkDeviceSize bytes,VkBufferUsageFlags usage,bool host=false) {
        buffers.push_back({}); auto& result=buffers.back(); result.bytes=bytes; result.usage=usage;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; info.size=bytes; info.usage=usage;
        info.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(device,&info,nullptr,&result.handle),"Create stream-test buffer");
        VkMemoryRequirements requirements{}; vkGetBufferMemoryRequirements(device,result.handle,&requirements);
        result.memory=allocate(requirements,host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkBindBufferMemory(device,result.handle,result.memory,0),"Bind stream-test buffer");
        if (host) check(vkMapMemory(device,result.memory,0,bytes,0,&result.mapping),"Map diagnostic readback");
        return result;
    }
    VkShaderModule module(std::span<const std::uint32_t> code) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; info.codeSize=code.size_bytes(); info.pCode=code.data();
        VkShaderModule result{}; check(vkCreateShaderModule(device,&info,nullptr,&result),"Create native draw shader"); return result;
    }
    void drawing(std::span<const std::uint32_t> vertex,std::span<const std::uint32_t> fragment) {
        VkFormatProperties formats{}; vkGetPhysicalDeviceFormatProperties(physical,VK_FORMAT_R8G8B8A8_UNORM,&formats);
        require((formats.optimalTilingFeatures&(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT))==
            (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT),"Missing color/readback format support");
        VkAttachmentDescription color{}; color.format=VK_FORMAT_R8G8B8A8_UNORM; color.samples=VK_SAMPLE_COUNT_1_BIT;
        color.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; color.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE; color.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout=color.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference reference{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{}; subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount=1; subpass.pColorAttachments=&reference;
        VkRenderPassCreateInfo pass_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        pass_info.attachmentCount=1; pass_info.pAttachments=&color; pass_info.subpassCount=1; pass_info.pSubpasses=&subpass;
        check(vkCreateRenderPass(device,&pass_info,nullptr,&pass),"Create stream draw pass");
        VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        check(vkCreatePipelineLayout(device,&layout_info,nullptr,&layout),"Create native draw layout");
        VkShaderModule vs{},fs{};
        try {
            vs=module(vertex); fs=module(fragment);
            std::array<VkPipelineShaderStageCreateInfo,2> stages{};
            stages[0]={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_VERTEX_BIT,vs,"main",nullptr};
            stages[1]={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_FRAGMENT_BIT,fs,"main",nullptr};
            VkVertexInputBindingDescription binding{0,16,VK_VERTEX_INPUT_RATE_VERTEX};
            VkVertexInputAttributeDescription attribute{0,0,VK_FORMAT_R32G32B32A32_SFLOAT,0};
            VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            input.vertexBindingDescriptionCount=1; input.pVertexBindingDescriptions=&binding;
            input.vertexAttributeDescriptionCount=1; input.pVertexAttributeDescriptions=&attribute;
            VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkViewport viewport{0,0,float(width),float(height),0,1}; VkRect2D scissor{{0,0},{width,height}};
            VkPipelineViewportStateCreateInfo viewport_info{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            viewport_info.viewportCount=1; viewport_info.pViewports=&viewport; viewport_info.scissorCount=1; viewport_info.pScissors=&scissor;
            VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            raster.polygonMode=VK_POLYGON_MODE_FILL; raster.cullMode=VK_CULL_MODE_NONE; raster.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE; raster.lineWidth=1;
            VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; samples.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
            VkPipelineColorBlendAttachmentState attachment{}; attachment.colorWriteMask=0xf;
            VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blend.attachmentCount=1; blend.pAttachments=&attachment;
            VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            info.stageCount=2; info.pStages=stages.data(); info.pVertexInputState=&input; info.pInputAssemblyState=&assembly;
            info.pViewportState=&viewport_info; info.pRasterizationState=&raster; info.pMultisampleState=&samples;
            info.pColorBlendState=&blend; info.layout=layout; info.renderPass=pass;
            check(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&info,nullptr,&pipeline),"Create direct stream draw pipeline");
        } catch (...) {
            if (vs) vkDestroyShaderModule(device,vs,nullptr);
            if (fs) vkDestroyShaderModule(device,fs,nullptr);
            throw;
        }
        vkDestroyShaderModule(device,vs,nullptr); vkDestroyShaderModule(device,fs,nullptr);
    }
    Image image() {
        images.push_back({}); auto& result=images.back();
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType=VK_IMAGE_TYPE_2D; info.format=VK_FORMAT_R8G8B8A8_UNORM; info.extent={width,height,1};
        info.mipLevels=1; info.arrayLayers=1; info.samples=VK_SAMPLE_COUNT_1_BIT; info.tiling=VK_IMAGE_TILING_OPTIMAL;
        info.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        check(vkCreateImage(device,&info,nullptr,&result.handle),"Create headless color image");
        VkMemoryRequirements requirements{}; vkGetImageMemoryRequirements(device,result.handle,&requirements);
        result.memory=allocate(requirements,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkBindImageMemory(device,result.handle,result.memory,0),"Bind headless image");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image=result.handle; view.viewType=VK_IMAGE_VIEW_TYPE_2D; view.format=info.format;
        view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        check(vkCreateImageView(device,&view,nullptr,&result.view),"Create headless image view");
        VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebuffer.renderPass=pass; framebuffer.attachmentCount=1; framebuffer.pAttachments=&result.view;
        framebuffer.width=width; framebuffer.height=height; framebuffer.layers=1;
        check(vkCreateFramebuffer(device,&framebuffer,nullptr,&result.framebuffer),"Create headless framebuffer");
        return result;
    }
};
void dependency(VkCommandBuffer command,const Buffer& buffer,VkPipelineStageFlags src_stage,VkAccessFlags src,
    VkPipelineStageFlags dst_stage,VkAccessFlags dst) {
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask=src; barrier.dstAccessMask=dst; barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer=buffer.handle; barrier.offset=0; barrier.size=buffer.bytes;
    vkCmdPipelineBarrier(command,src_stage,dst_stage,0,0,nullptr,1,&barrier,0,nullptr);
}
void draw(Device& device,VkCommandBuffer command,Image& image,RecordedVertices vertices,const Buffer& readback,VkDeviceSize offset) {
    VkImageMemoryBarrier before{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    before.srcAccessMask=image.state==VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_TRANSFER_READ_BIT;
    before.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; before.oldLayout=image.state;
    before.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    before.srcQueueFamilyIndex=before.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    before.image=image.handle; before.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdPipelineBarrier(command,image.state==VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,nullptr,0,nullptr,1,&before);
    VkClearValue clear{}; clear.color.float32[3]=1;
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass=device.pass; begin.framebuffer=image.framebuffer; begin.renderArea={{0,0},{width,height}};
    begin.clearValueCount=1; begin.pClearValues=&clear;
    vkCmdBeginRenderPass(command,&begin,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,device.pipeline);
    vkCmdBindVertexBuffers(command,0,1,&vertices.buffer,&vertices.offset);
    vkCmdDraw(command,vertices.count,1,0,0);
    vkCmdEndRenderPass(command);
    auto after=before; after.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; after.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    after.oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; after.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&after);
    VkBufferImageCopy copy{}; copy.bufferOffset=offset; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={width,height,1};
    vkCmdCopyImageToBuffer(command,image.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback.handle,1,&copy);
    image.state=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
}
std::vector<std::uint32_t> inputs(unsigned variant,unsigned count,unsigned pass,std::vector<std::uint32_t>& expected) {
    const auto c=render_fixture::configuration(variant);
    std::array<float,16> matrix{}; matrix[0]=pass==1 ? 0.75f : 1.0f; matrix[5]=1; matrix[10]=1; matrix[15]=1;
    matrix[12]=pass==1 ? 0.125f : 0; matrix[13]=pass==2 ? -0.125f : 0;
    std::vector<std::uint32_t> source(4096);
    const auto matrix_row=variant ? 8U : 0U;
    for (unsigned n=0;n<16;++n) source[matrix_row*4+n]=std::bit_cast<std::uint32_t>(matrix[n]);
    expected.resize(count*4);
    for (unsigned n=0;n<count;++n) {
        const unsigned triangle=n/3,corner=n%3;
        const float x=-0.875f+float(triangle%8)*0.25f;
        const float y=-0.875f+float(triangle/8)*0.25f;
        const std::array vertex{x+(corner==1 ? 0.125f : 0.0f),y+(corner==2 ? 0.125f : 0.0f),0.5f,1.0f};
        for (unsigned lane=0;lane<4;++lane) {
            source[(16+n*c.input_stride)*4+lane]=std::bit_cast<std::uint32_t>(vertex[lane]);
            volatile float sum=matrix[lane]*vertex[0];
            for (unsigned column=1;column<4;++column) {
                volatile float product=matrix[column*4+lane]*vertex[column]; sum=sum+product;
            }
            expected[n*4+lane]=std::bit_cast<std::uint32_t>(float(sum));
        }
    }
    return source;
}
void run(std::span<const std::uint32_t> compute,std::span<const std::uint32_t> vertex,
    std::span<const std::uint32_t> fragment,unsigned variant) {
    Device device; device.initialize(); device.drawing(vertex,fragment);
    auto actual_image=device.image(),reference_image=device.image();
    const auto source=device.buffer(16384,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    const auto padding=std::max<VkDeviceSize>(16,device.properties.limits.minStorageBufferOffsetAlignment);
    const auto output=device.buffer(padding+(maximum_items+64)*16,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    const auto reference=device.buffer(maximum_items*16,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    const auto frame_bytes=2*pixel_bytes+output.bytes;
    const auto readback=device.buffer(frame_bytes*3,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true);
    const auto loop=render_fixture::loop(variant);
    auto kernel=std::make_shared<VulkanRenderKernel>(device.physical,device.device,compute,nt::render::render_stream_layout(loop));
    auto stream=std::make_unique<VulkanRenderStream>(kernel,source.view(),output.view(padding));
    // Exception paths must also retire submitted work before stream destruction.
    struct Drain { VkDevice device; ~Drain() { vkDeviceWaitIdle(device); } } drain{device.device};
    bool rejected=false;
    try { VulkanRenderStream alias(kernel,source.view(),source.view()); }
    catch (const std::invalid_argument&) { rejected=true; }
    require(rejected,"Aliased Vulkan storage was accepted");
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocation.commandPool=device.pool; allocation.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocation.commandBufferCount=1;
    VkCommandBuffer command{}; check(vkAllocateCommandBuffers(device.device,&allocation,&command),"Allocate direct draw commands");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    check(vkBeginCommandBuffer(command,&begin),"Begin direct draw commands");
    const std::array counts{3U,66U,129U};
    std::array<std::vector<std::uint32_t>,3> expected;
    for (unsigned pass=0;pass<counts.size();++pass) {
        auto data=inputs(variant,counts[pass],pass,expected[pass]);
        const auto bound=nt::render::bind_vertex_loop(loop,render_fixture::integers(variant,counts[pass]),data);
        auto invalid=bound.parameters; invalid[0]=0;
        rejected=false; try { (void)stream->record(command,invalid); } catch (const std::invalid_argument&) { rejected=true; }
        require(rejected,"Invalid dispatch was recorded");
        dependency(command,source,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdUpdateBuffer(command,source.handle,0,data.size()*4,data.data());
        dependency(command,output,VK_PIPELINE_STAGE_VERTEX_INPUT_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdFillBuffer(command,output.handle,0,output.bytes,sentinel);
        dependency(command,output,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT);
        const auto vertices=stream->record(command,bound.parameters,StreamSourceProducer::transfer);
        require(vertices.buffer==output.handle && vertices.offset==padding && vertices.count==counts[pass],"Draw is not using the compute output allocation");
        // The draw consumes the device buffer immediately; no CPU vertex path.
        draw(device,command,actual_image,vertices,readback,pass*frame_bytes);
        // Diagnostic copies happen AFTER the actual draw, never to feed it.
        dependency(command,output,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_VERTEX_INPUT_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferCopy copy{0,pass*frame_bytes+2*pixel_bytes,output.bytes};
        vkCmdCopyBuffer(command,output.handle,readback.handle,1,&copy);
        dependency(command,reference,VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdUpdateBuffer(command,reference.handle,0,expected[pass].size()*4,expected[pass].data());
        dependency(command,reference,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT);
        draw(device,command,reference_image,{reference.handle,0,counts[pass]},readback,pass*frame_bytes+pixel_bytes);
    }
    dependency(command,readback,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_READ_BIT);
    check(vkEndCommandBuffer(command),"End direct draw commands");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&command;
    check(vkQueueSubmit(device.queue,1,&submit,device.fence),"Submit generated compute and native draws");
    // One completion wait for the whole test, not between transforms and draws.
    check(vkWaitForFences(device.device,1,&device.fence,VK_TRUE,30000000000ULL),"Wait for direct draw test");
    auto* bytes=static_cast<const std::uint8_t*>(readback.mapping);
    for (unsigned pass=0;pass<counts.size();++pass) {
        const auto* actual=bytes+pass*frame_bytes;
        require(std::memcmp(actual,actual+pixel_bytes,pixel_bytes)==0,"GPU-stream draw differs from independent CPU-transform draw");
        unsigned colored=0;
        for (unsigned n=0;n<pixel_bytes;n+=4) if (actual[n] || actual[n+1] || actual[n+2]) ++colored;
        require(colored>0,"Both reference and actual image were empty");
        const auto* words=reinterpret_cast<const std::uint32_t*>(actual+2*pixel_bytes);
        for (VkDeviceSize n=0;n<output.bytes/4;++n) {
            const auto first=padding/4;
            const auto wanted=n>=first && n-first<expected[pass].size() ? expected[pass][static_cast<std::size_t>(n-first)] : sentinel;
            if (words[n]!=wanted) throw std::runtime_error("Generated GPU value or guard mismatch in frame "+std::to_string(pass)+", word "+std::to_string(n));
        }
    }
    stream.reset(); kernel.reset(); device.clear();
    require(device.errors.load()==0,"Vulkan core or synchronization validation reported an error");
    std::cout<<"Three GPU-resident compute-to-draw batches matched CPU reference pixels and output guards; zero validation errors\n";
}
}
int main(int argc,char** argv) {
    try {
        require(argc==5,"Expected generated compute SPIR-V, vertex SPIR-V, fragment SPIR-V and fixture variant");
        unsigned variant=0; const std::string text=argv[4];
        const auto parsed=std::from_chars(text.data(),text.data()+text.size(),variant);
        require(parsed.ec==std::errc{} && parsed.ptr==text.data()+text.size() && variant<2,"Invalid fixture variant");
        run(shader(argv[1]),shader(argv[2]),shader(argv[3]),variant); return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
