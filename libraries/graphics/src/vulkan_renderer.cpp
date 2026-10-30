#include <nt/vulkan_renderer.hpp>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace nt::graphics {
namespace {
void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation)+": Vulkan result "+std::to_string(result));
}
constexpr VkDeviceSize memory_bytes=4*1024*1024;
constexpr VkDeviceSize buffer_bytes=memory_bytes*2+16*sizeof(std::uint32_t);
const std::uint32_t shader_code[] =
#include "sprite.inc"
;
}
struct VulkanRenderer::Impl {
    VkInstance instance{};
    VkDebugUtilsMessengerEXT debug{};
    VkSurfaceKHR surface{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    unsigned family=0;
    VkBuffer buffer{};
    VkDeviceMemory allocation{};
    void* mapped=nullptr;
    VkDescriptorSetLayout set_layout{};
    VkDescriptorPool descriptors{};
    VkDescriptorSet set{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    VkCommandPool pool{};
    VkCommandBuffer command{};
    VkFence fence{};
    VkSwapchainKHR swapchain{};
    VkSemaphore acquired{}, rendered{};
    VkFormat format{};
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::string name;
    std::atomic<unsigned> errors=0;
    static VKAPI_ATTR VkBool32 VKAPI_CALL report(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT*, void* user) {
        if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++static_cast<Impl*>(user)->errors;
        return VK_FALSE;
    }
    ~Impl() {
        if (device) vkDeviceWaitIdle(device);
        if (mapped) vkUnmapMemory(device,allocation);
        if (rendered) vkDestroySemaphore(device,rendered,nullptr);
        if (acquired) vkDestroySemaphore(device,acquired,nullptr);
        if (swapchain) vkDestroySwapchainKHR(device,swapchain,nullptr);
        if (fence) vkDestroyFence(device,fence,nullptr);
        if (pool) vkDestroyCommandPool(device,pool,nullptr);
        if (pipeline) vkDestroyPipeline(device,pipeline,nullptr);
        if (layout) vkDestroyPipelineLayout(device,layout,nullptr);
        if (descriptors) vkDestroyDescriptorPool(device,descriptors,nullptr);
        if (set_layout) vkDestroyDescriptorSetLayout(device,set_layout,nullptr);
        if (buffer) vkDestroyBuffer(device,buffer,nullptr);
        if (allocation) vkFreeMemory(device,allocation,nullptr);
        if (device) vkDestroyDevice(device,nullptr);
        if (surface) vkDestroySurfaceKHR(instance,surface,nullptr);
        if (debug) reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT"))(instance,debug,nullptr);
        if (instance) vkDestroyInstance(instance,nullptr);
    }
    void initialize(void* window, bool validation) {
        std::vector<const char*> extensions;
        if (validation) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#ifdef _WIN32
        if (window) { extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME); extensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME); }
#else
        if (window) throw std::runtime_error("Windowed Vulkan host currently requires Windows");
#endif
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName="AnyPS2"; app.apiVersion=VK_API_VERSION_1_1;
        const char* layer="VK_LAYER_KHRONOS_validation";
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        info.pApplicationInfo=&app; info.enabledExtensionCount=static_cast<unsigned>(extensions.size()); info.ppEnabledExtensionNames=extensions.data();
        info.enabledLayerCount=validation?1:0; info.ppEnabledLayerNames=&layer;
        check(vkCreateInstance(&info,nullptr,&instance),"Create instance");
        if (validation) {
            VkDebugUtilsMessengerCreateInfoEXT d{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            d.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            d.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            d.pfnUserCallback=report; d.pUserData=this;
            check(reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkCreateDebugUtilsMessengerEXT"))(instance,&d,nullptr,&debug),"Create validation messenger");
        }
#ifdef _WIN32
        if (window) {
            VkWin32SurfaceCreateInfoKHR s{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
            s.hinstance=GetModuleHandleW(nullptr); s.hwnd=static_cast<HWND>(window);
            check(vkCreateWin32SurfaceKHR(instance,&s,nullptr,&surface),"Create window surface");
        }
#endif
        unsigned count=0; check(vkEnumeratePhysicalDevices(instance,&count,nullptr),"Enumerate GPUs");
        std::vector<VkPhysicalDevice> devices(count); check(vkEnumeratePhysicalDevices(instance,&count,devices.data()),"Enumerate GPUs");
        int best=-1;
        for (auto candidate:devices) {
            VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(candidate,&props);
            unsigned n=0; vkGetPhysicalDeviceQueueFamilyProperties(candidate,&n,nullptr);
            std::vector<VkQueueFamilyProperties> queues(n); vkGetPhysicalDeviceQueueFamilyProperties(candidate,&n,queues.data());
            for (unsigned i=0;i<n;++i) {
                VkBool32 present=VK_TRUE;
                if (surface) check(vkGetPhysicalDeviceSurfaceSupportKHR(candidate,i,surface,&present),"Query presentation support");
                const int score=props.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU?2:1;
                if ((queues[i].queueFlags&VK_QUEUE_COMPUTE_BIT) && present && score>best) {
                    physical=candidate; family=i; name=props.deviceName; best=score;
                }
            }
        }
        if (!physical) throw std::runtime_error("No Vulkan compute/presentation device");
        const float priority=1;
        VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; q.queueFamilyIndex=family; q.queueCount=1; q.pQueuePriorities=&priority;
        const char* swap_extension=VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dc.queueCreateInfoCount=1; dc.pQueueCreateInfos=&q;
        dc.enabledExtensionCount=surface?1:0; dc.ppEnabledExtensionNames=&swap_extension;
        check(vkCreateDevice(physical,&dc,nullptr,&device),"Create device"); vkGetDeviceQueue(device,family,0,&queue);
        VkBufferCreateInfo b{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; b.size=buffer_bytes; b.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        check(vkCreateBuffer(device,&b,nullptr,&buffer),"Create GS buffer");
        VkMemoryRequirements requirements; vkGetBufferMemoryRequirements(device,buffer,&requirements);
        VkPhysicalDeviceMemoryProperties memory; vkGetPhysicalDeviceMemoryProperties(physical,&memory);
        unsigned type=memory.memoryTypeCount;
        for (unsigned i=0;i<memory.memoryTypeCount;++i)
            if ((requirements.memoryTypeBits&(1U<<i)) && (memory.memoryTypes[i].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {type=i;break;}
        if (type==memory.memoryTypeCount) throw std::runtime_error("No coherent host-visible Vulkan memory");
        VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; a.allocationSize=requirements.size; a.memoryTypeIndex=type;
        check(vkAllocateMemory(device,&a,nullptr,&allocation),"Allocate GS buffer");
        check(vkBindBufferMemory(device,buffer,allocation,0),"Bind GS buffer"); check(vkMapMemory(device,allocation,0,buffer_bytes,0,&mapped),"Map GS buffer");
        VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; sl.bindingCount=1; sl.pBindings=&binding;
        check(vkCreateDescriptorSetLayout(device,&sl,nullptr,&set_layout),"Create descriptor layout");
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dp.maxSets=1; dp.poolSizeCount=1; dp.pPoolSizes=&ps;
        check(vkCreateDescriptorPool(device,&dp,nullptr,&descriptors),"Create descriptors");
        VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; da.descriptorPool=descriptors; da.descriptorSetCount=1; da.pSetLayouts=&set_layout;
        check(vkAllocateDescriptorSets(device,&da,&set),"Allocate descriptors");
        VkDescriptorBufferInfo bi{buffer,0,buffer_bytes};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; write.dstSet=set; write.descriptorCount=1; write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.pBufferInfo=&bi;
        vkUpdateDescriptorSets(device,1,&write,0,nullptr);
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(Sprite)};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pl.setLayoutCount=1; pl.pSetLayouts=&set_layout; pl.pushConstantRangeCount=1; pl.pPushConstantRanges=&push;
        check(vkCreatePipelineLayout(device,&pl,nullptr,&layout),"Create pipeline layout");
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; sm.codeSize=sizeof(shader_code); sm.pCode=shader_code;
        VkShaderModule module{}; check(vkCreateShaderModule(device,&sm,nullptr,&module),"Create sprite shader");
        VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; cp.layout=layout;
        cp.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; cp.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; cp.stage.module=module; cp.stage.pName="main";
        const auto pipeline_result=vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&cp,nullptr,&pipeline);
        vkDestroyShaderModule(device,module,nullptr); check(pipeline_result,"Create sprite pipeline");
        VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pc.queueFamilyIndex=family; pc.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        check(vkCreateCommandPool(device,&pc,nullptr,&pool),"Create command pool");
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool=pool; ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount=1;
        check(vkAllocateCommandBuffers(device,&ca,&command),"Allocate command buffer");
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; check(vkCreateFence(device,&fi,nullptr,&fence),"Create fence");
        if (surface) create_swapchain();
    }
    void create_swapchain() {
        VkSurfaceCapabilitiesKHR caps; check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical,surface,&caps),"Query surface");
        unsigned count=0; check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&count,nullptr),"Query formats");
        std::vector<VkSurfaceFormatKHR> formats(count); check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&count,formats.data()),"Query formats");
        auto selected=std::find_if(formats.begin(),formats.end(),[](auto f){return (f.format==VK_FORMAT_B8G8R8A8_UNORM || f.format==VK_FORMAT_R8G8B8A8_UNORM) && f.colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;});
        if (selected==formats.end() || !(caps.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_DST_BIT)) throw std::runtime_error("No supported Vulkan presentation format/transfer usage");
        if (!(caps.supportedCompositeAlpha&VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) throw std::runtime_error("Opaque presentation unsupported");
        extent=caps.currentExtent; format=selected->format;
        if (!extent.width || !extent.height || std::uint64_t(extent.width)*extent.height*4>memory_bytes) throw std::runtime_error("Unsupported presentation extent");
        VkSwapchainCreateInfoKHR sc{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR}; sc.surface=surface;
        sc.minImageCount=std::min(caps.minImageCount+1,caps.maxImageCount?caps.maxImageCount:~0U);
        sc.imageFormat=format; sc.imageColorSpace=selected->colorSpace; sc.imageExtent=extent; sc.imageArrayLayers=1; sc.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        sc.preTransform=caps.currentTransform; sc.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR; sc.presentMode=VK_PRESENT_MODE_FIFO_KHR; sc.clipped=VK_TRUE;
        check(vkCreateSwapchainKHR(device,&sc,nullptr,&swapchain),"Create swapchain");
        check(vkGetSwapchainImagesKHR(device,swapchain,&count,nullptr),"Get swapchain images"); images.resize(count); check(vkGetSwapchainImagesKHR(device,swapchain,&count,images.data()),"Get swapchain images");
        VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; check(vkCreateSemaphore(device,&sem,nullptr,&acquired),"Create acquire semaphore"); check(vkCreateSemaphore(device,&sem,nullptr,&rendered),"Create render semaphore");
    }
    void begin() {
        check(vkResetCommandBuffer(command,0),"Reset commands");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(command,&begin),"Begin commands");
    }
    void submit(bool display=false) {
        check(vkEndCommandBuffer(command),"End commands"); check(vkResetFences(device,1,&fence),"Reset fence");
        VkPipelineStageFlags stage=VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO}; s.commandBufferCount=1; s.pCommandBuffers=&command;
        if (display) {s.waitSemaphoreCount=1;s.pWaitSemaphores=&acquired;s.pWaitDstStageMask=&stage;s.signalSemaphoreCount=1;s.pSignalSemaphores=&rendered;}
        check(vkQueueSubmit(queue,1,&s,fence),"Submit commands");
        check(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_MAX),"Wait for GPU");
    }
};
VulkanRenderer::VulkanRenderer(void* window,bool validation):impl_(std::make_unique<Impl>()) {impl_->initialize(window,validation);}
VulkanRenderer::~VulkanRenderer()=default;
void VulkanRenderer::synchronize() { check(vkDeviceWaitIdle(impl_->device),"Synchronize graphics"); }
const std::string& VulkanRenderer::device_name() const {return impl_->name;}
unsigned VulkanRenderer::validation_errors() const {return impl_->errors.load();}
void VulkanRenderer::sprite(std::span<std::uint32_t> memory,const Sprite& draw,std::span<const std::uint32_t> palette) {
    if ((draw.flags&64) && palette.size()!=16) throw std::runtime_error("PSMT4 requires sixteen palette entries");
    if (memory.size_bytes()!=memory_bytes || draw.right>2048 || draw.bottom>2048 || draw.left>draw.right || draw.top>draw.bottom || !draw.pitch || draw.pitch>63 || draw.base>16383)
        throw std::runtime_error("Invalid Vulkan sprite bounds or GS memory");
    if (draw.left==draw.right || draw.top==draw.bottom) return;
    if ((draw.flags&256) || (draw.test&0x4000)) {
        if (draw.right>draw.pitch*64 || ((draw.bottom-1)/32-draw.top/32)*draw.pitch+(draw.right-1)/64-draw.left/64>=512)
            throw std::runtime_error("Destination-dependent sprite requires non-aliased framebuffer");
        if (draw.flags&256) for (unsigned shift=0;shift<8;shift+=2)
            if (((draw.blend>>shift)&3)>2) throw std::runtime_error("Unsupported Vulkan blend selector");
    }
    if ((draw.flags&1) && (!draw.dx || !draw.dy || !draw.texture_pitch || !draw.texture_width ||
        !draw.texture_height || draw.right>draw.pitch*64 || ((draw.bottom-1)/32-draw.top/32)*draw.pitch+(draw.right-1)/64-draw.left/64>=512))
        throw std::runtime_error("Unsupported textured sprite dimensions or aliased framebuffer");
    if (draw.flags&1) {
        const auto power_of_two=[](unsigned n){return n && !(n&(n-1)) && n<=1024;};
        const auto coordinate=[](int n){return n>=-65535 && n<=65535;};
        const auto delta=[](int n){return n>=-16383 && n<=16383;};
        if (draw.texture_base>16383 || draw.texture_pitch>63 || !power_of_two(draw.texture_width) ||
            !power_of_two(draw.texture_height) || ((draw.flags>>3)&3)>1 || draw.clamp_u>3 || draw.clamp_v>3 ||
            draw.min_u>1023 || draw.max_u>1023 || draw.min_v>1023 || draw.max_v>1023 ||
            (draw.clamp_u==2 && draw.min_u>draw.max_u) || (draw.clamp_v==2 && draw.min_v>draw.max_v) ||
            !coordinate(draw.x0) || !coordinate(draw.y0) || !coordinate(draw.dx) || !coordinate(draw.dy) ||
            draw.u0<0 || draw.u0>16383 || draw.v0<0 || draw.v0>16383 || !delta(draw.du) || !delta(draw.dv))
            throw std::runtime_error("Unsupported Vulkan texture parameters");
    }
    auto& v=*impl_; std::memcpy(v.mapped,memory.data(),memory_bytes);
    if (draw.flags&1) std::memcpy(static_cast<std::uint8_t*>(v.mapped)+memory_bytes,memory.data(),memory_bytes);
    if (draw.flags&64) std::memcpy(static_cast<std::uint8_t*>(v.mapped)+memory_bytes*2,palette.data(),64);
    v.begin();
    vkCmdBindPipeline(v.command,VK_PIPELINE_BIND_POINT_COMPUTE,v.pipeline);
    vkCmdBindDescriptorSets(v.command,VK_PIPELINE_BIND_POINT_COMPUTE,v.layout,0,1,&v.set,0,nullptr);
    vkCmdPushConstants(v.command,v.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(draw),&draw);
    vkCmdDispatch(v.command,(draw.right-draw.left+7)/8,(draw.bottom-draw.top+7)/8,1);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(v.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    v.submit(); std::memcpy(memory.data(),v.mapped,memory_bytes);
}
void VulkanRenderer::present(std::span<const std::uint32_t> rgba,unsigned width,unsigned height) {
    auto& v=*impl_;
    if (!v.swapchain || width!=v.extent.width || height!=v.extent.height || rgba.size()!=std::size_t(width)*height) throw std::runtime_error("Presentation size must match the fixed window");
    unsigned index=0; check(vkAcquireNextImageKHR(v.device,v.swapchain,UINT64_MAX,v.acquired,VK_NULL_HANDLE,&index),"Acquire image");
    auto* output=static_cast<std::uint32_t*>(v.mapped);
    for (std::size_t i=0;i<rgba.size();++i) {auto p=rgba[i];output[i]=v.format==VK_FORMAT_B8G8R8A8_UNORM ? (p&0xff00ff00U)|((p&255)<<16)|((p>>16)&255) : p;}
    v.begin();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; b.image=v.images[index]; b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdPipelineBarrier(v.command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
    VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={width,height,1};
    vkCmdCopyBufferToImage(v.command,v.buffer,v.images[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=0;
    vkCmdPipelineBarrier(v.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&b);
    v.submit(true);
    VkPresentInfoKHR p{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};p.waitSemaphoreCount=1;p.pWaitSemaphores=&v.rendered;p.swapchainCount=1;p.pSwapchains=&v.swapchain;p.pImageIndices=&index;
    check(vkQueuePresentKHR(v.queue,&p),"Present image"); check(vkQueueWaitIdle(v.queue),"Wait for presentation");
}
}
