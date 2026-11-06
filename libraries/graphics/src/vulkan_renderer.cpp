#include <nt/vulkan_renderer.hpp>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>
#include "native_raster.hpp"
#include "sprite_mapping.hpp"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <cstring>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace nt::graphics {
namespace {
void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation)+": Vulkan result "+std::to_string(result));
}
constexpr VkDeviceSize memory_bytes=4*1024*1024;
// Internal shader flag: only the renderer may select direct texture reads,
// after proving the current draw cannot write any sampled storage.
constexpr std::uint32_t direct_texture_flag=1U<<22;
struct StorageRange {
    VkDeviceSize offset,length;
};
std::array<StorageRange,2> split_storage(StorageRange range) {
    range.offset%=memory_bytes;
    range.length=std::min(range.length,memory_bytes);
    const auto first=std::min(range.length,memory_bytes-range.offset);
    return {{{range.offset,first},{0,range.length-first}}};
}
bool overlaps(StorageRange a,StorageRange b) {
    for (const auto left:split_storage(a)) for (const auto right:split_storage(b))
        if (left.length && right.length && left.offset<right.offset+right.length &&
            right.offset<left.offset+left.length) return true;
    return false;
}
StorageRange texture_storage(const Sprite& draw) {
    const auto maximum=[](unsigned size,unsigned mode,unsigned low,unsigned high) {
        return mode==2 ? high : mode==3 ? (low|high) : size-1;
    };
    const auto x=maximum(draw.texture_width,draw.clamp_u,draw.min_u,draw.max_u);
    const auto y=maximum(draw.texture_height,draw.clamp_v,draw.min_v,draw.max_v);
    const auto page=(draw.flags&64) && !(draw.flags&texture_high_flag) ? (y/((draw.flags&indexed8_flag) ? 64 : 128))*(draw.texture_pitch/2)+x/128
                                    : (y/32)*draw.texture_pitch+x/64;
    return {VkDeviceSize(draw.texture_base)*256,std::min(memory_bytes,VkDeviceSize(page+1)*8192)};
}
StorageRange output_storage(const Sprite& draw,unsigned base) {
    const auto first=(draw.top/32)*draw.pitch+draw.left/64;
    const auto last=((draw.bottom-1)/32)*draw.pitch+(draw.right-1)/64;
    // Include full pages. Depth's XOR swizzle stays inside its aligned page.
    return {VkDeviceSize(base)*256+VkDeviceSize(first)*8192,VkDeviceSize(last-first+1)*8192};
}
constexpr VkDeviceSize triangle_offset=memory_bytes*2+256*sizeof(std::uint32_t);
constexpr VkDeviceSize writes_offset=memory_bytes*2+1280;
constexpr unsigned max_writes=65536;
constexpr VkDeviceSize display_offset=9*1024*1024+1024;
constexpr VkDeviceSize display_bytes=2048*2048*4;
constexpr VkDeviceSize buffer_bytes=display_offset+display_bytes;
static_assert(writes_offset+max_writes*sizeof(PixelWrite)<display_offset-4);
static_assert(sizeof(DisplayPlan)==116 && sizeof(DisplayPlan)<=128);
static_assert(sizeof(PixelWrite)==12);
static_assert(sizeof(RasterVertex)==40);
static_assert(sizeof(Sprite)==128);
#if ANYPS2_REFERENCE_RENDERER
const std::uint32_t shader_code[] =
#include "sprite.inc"
;
const std::uint32_t scanout_code[] =
#include "scanout.inc"
;
#endif
}
struct VulkanRenderer::Impl {
    std::unique_ptr<NativeRaster> native;
    RenderPath path=RenderPath::native_images;
    std::uint32_t api_version=VK_API_VERSION_1_1;
    bool shader_int8=false;
    VkInstance instance{};
    VkDebugUtilsMessengerEXT debug{};
    VkSurfaceKHR surface{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    unsigned family=0;
    VkBuffer buffer{},storage{};
    VkDeviceMemory allocation{},storage_allocation{};
    void* mapped=nullptr;
    VkDescriptorSetLayout set_layout{};
    VkDescriptorPool descriptors{};
    VkDescriptorSet set{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{},display_pipeline{};
    unsigned display_width=0,display_height=0,display_bgra=0;
    VkCommandPool pool{};
    VkCommandBuffer command{};
    VkFence fence{};
    VkSwapchainKHR swapchain{};
    VkSemaphore acquired{}, rendered{};
    struct PresentFrame {
        VkSemaphore acquired{};
        std::uint64_t submission=0;
    };
    std::array<PresentFrame,2> present_frames{};
    unsigned present_frame=0;
    std::vector<VkSemaphore> present_ready;
    VkFormat format{};
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::string name;
    std::atomic<unsigned> errors=0;
    mutable std::mutex validation_mutex;
    std::string first_validation_error;
    RenderStatistics statistics;
    bool recording=false,initialized=false;
    unsigned recorded=0;
    void order() {
        if (!recording) { begin(); recording=true; recorded=0; }
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        barrier.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0,1,&barrier,0,nullptr,0,nullptr);
    }
    void flush() {
        if (!recording) return;
        submit(); recording=false; recorded=0; ++statistics.submissions;
    }
    void bounded() { if (++recorded>=512) flush(); }
    void bind() {
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
        vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,nullptr);
    }
    static VKAPI_ATTR VkBool32 VKAPI_CALL report(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
        if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
            auto& self=*static_cast<Impl*>(user);
            ++self.errors;
            std::lock_guard lock(self.validation_mutex);
            if (self.first_validation_error.empty() && data && data->pMessage)
                self.first_validation_error=data->pMessage;
        }
        return VK_FALSE;
    }
    ~Impl() {
        if (device) vkDeviceWaitIdle(device);
        native.reset();
        if (mapped) vkUnmapMemory(device,allocation);
        if (rendered) vkDestroySemaphore(device,rendered,nullptr);
        if (acquired) vkDestroySemaphore(device,acquired,nullptr);
        for (const auto& frame:present_frames) if (frame.acquired) vkDestroySemaphore(device,frame.acquired,nullptr);
        for (auto ready:present_ready) if (ready) vkDestroySemaphore(device,ready,nullptr);
        if (swapchain) vkDestroySwapchainKHR(device,swapchain,nullptr);
        if (fence) vkDestroyFence(device,fence,nullptr);
        if (pool) vkDestroyCommandPool(device,pool,nullptr);
        if (display_pipeline) vkDestroyPipeline(device,display_pipeline,nullptr);
        if (pipeline) vkDestroyPipeline(device,pipeline,nullptr);
        if (layout) vkDestroyPipelineLayout(device,layout,nullptr);
        if (descriptors) vkDestroyDescriptorPool(device,descriptors,nullptr);
        if (set_layout) vkDestroyDescriptorSetLayout(device,set_layout,nullptr);
        if (buffer) vkDestroyBuffer(device,buffer,nullptr);
        if (storage) vkDestroyBuffer(device,storage,nullptr);
        if (allocation) vkFreeMemory(device,allocation,nullptr);
        if (storage_allocation) vkFreeMemory(device,storage_allocation,nullptr);
        if (device) vkDestroyDevice(device,nullptr);
        if (surface) vkDestroySurfaceKHR(instance,surface,nullptr);
        if (debug) reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT"))(instance,debug,nullptr);
        if (instance) vkDestroyInstance(instance,nullptr);
    }
    void create_buffer(VkBufferUsageFlags usage,VkMemoryPropertyFlags required,VkMemoryPropertyFlags preferred,
                       VkBuffer& result,VkDeviceMemory& memory) {
        VkBufferCreateInfo b{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; b.size=buffer_bytes; b.usage=usage;
        check(vkCreateBuffer(device,&b,nullptr,&result),"Create graphics buffer");
        VkMemoryRequirements requirements; vkGetBufferMemoryRequirements(device,result,&requirements);
        VkPhysicalDeviceMemoryProperties properties; vkGetPhysicalDeviceMemoryProperties(physical,&properties);
        unsigned type=properties.memoryTypeCount;
        for (unsigned i=0;i<properties.memoryTypeCount;++i) {
            const auto flags=properties.memoryTypes[i].propertyFlags;
            if (!(requirements.memoryTypeBits&(1U<<i)) || (flags&required)!=required) continue;
            if (type==properties.memoryTypeCount) type=i;
            if ((flags&preferred)==preferred) { type=i; break; }
        }
        if (type==properties.memoryTypeCount) throw std::runtime_error("No compatible Vulkan buffer memory");
        VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        a.allocationSize=requirements.size; a.memoryTypeIndex=type;
        check(vkAllocateMemory(device,&a,nullptr,&memory),"Allocate graphics buffer");
        check(vkBindBufferMemory(device,result,memory,0),"Bind graphics buffer");
    }
    void initialize(void* window, bool validation) {
#if !ANYPS2_REFERENCE_RENDERER
        if (path!=RenderPath::native_images) throw std::runtime_error("Reference compute renderer is not part of native output");
#endif
        std::vector<const char*> extensions;
        if (validation) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#ifdef _WIN32
        if (window) { extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME); extensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME); }
#else
        if (window) throw std::runtime_error("Windowed Vulkan host currently requires Windows");
#endif
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        std::uint32_t loader_version=VK_API_VERSION_1_0;
        if (const auto enumerate=reinterpret_cast<PFN_vkEnumerateInstanceVersion>(vkGetInstanceProcAddr(nullptr,"vkEnumerateInstanceVersion")))
            check(enumerate(&loader_version),"Query Vulkan version");
        if (loader_version<VK_API_VERSION_1_1) throw std::runtime_error("Native rendering requires Vulkan 1.1 or newer");
        app.pApplicationName="AnyPS2"; app.apiVersion=std::min(loader_version,VK_API_VERSION_1_3);
#ifdef ANYPS2_LLVM_TEXTURES
        app.apiVersion=VK_API_VERSION_1_3;
#endif
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
#ifdef ANYPS2_LLVM_TEXTURES
            if (props.apiVersion<VK_API_VERSION_1_3) continue;
#endif
            unsigned n=0; vkGetPhysicalDeviceQueueFamilyProperties(candidate,&n,nullptr);
            std::vector<VkQueueFamilyProperties> queues(n); vkGetPhysicalDeviceQueueFamilyProperties(candidate,&n,queues.data());
            for (unsigned i=0;i<n;++i) {
                VkBool32 present=VK_TRUE;
                if (surface) check(vkGetPhysicalDeviceSurfaceSupportKHR(candidate,i,surface,&present),"Query presentation support");
                const int score=props.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU?2:1;
                if ((queues[i].queueFlags&VK_QUEUE_COMPUTE_BIT) && (queues[i].queueFlags&VK_QUEUE_GRAPHICS_BIT) && present && score>best) {
                    physical=candidate; family=i; name=props.deviceName; best=score;
                }
            }
        }
        if (!physical) throw std::runtime_error("No Vulkan compute/presentation device");
        VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(physical,&properties);
        api_version=std::min(app.apiVersion,properties.apiVersion);
        const float priority=1;
        VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; q.queueFamilyIndex=family; q.queueCount=1; q.pQueuePriorities=&priority;
        const char* swap_extension=VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        VkPhysicalDeviceFeatures available{};
        vkGetPhysicalDeviceFeatures(physical,&available);
        if (!available.shaderInt64) throw std::runtime_error("Native triangle coverage requires Vulkan shaderInt64");
        VkPhysicalDeviceFeatures features{}; features.shaderInt64=VK_TRUE;
        VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dc.queueCreateInfoCount=1; dc.pQueueCreateInfos=&q;
        dc.pEnabledFeatures=&features;
        std::vector<const char*> device_extensions;
        VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT interlock{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
        bool ordered_fragments=false;
        if (surface) device_extensions.push_back(swap_extension);
        if (path==RenderPath::native_images) {
            unsigned n=0; check(vkEnumerateDeviceExtensionProperties(physical,nullptr,&n,nullptr),"Query native device extensions");
            std::vector<VkExtensionProperties> properties(n);
            check(vkEnumerateDeviceExtensionProperties(physical,nullptr,&n,properties.data()),"Query native device extensions");
            if (std::none_of(properties.begin(),properties.end(),[](auto& p){return std::strcmp(p.extensionName,VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME)==0;}))
                throw std::runtime_error("Native AA coverage requires VK_EXT_conservative_rasterization; no compute fallback is used");
            device_extensions.push_back(VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME);
            if (available.fragmentStoresAndAtomics && std::any_of(properties.begin(),properties.end(),[](auto& p){
                    return std::strcmp(p.extensionName,VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME)==0;})) {
                VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; query.pNext=&interlock;
                vkGetPhysicalDeviceFeatures2(physical,&query);
                ordered_fragments=interlock.fragmentShaderPixelInterlock;
                if (ordered_fragments) {
                    interlock.fragmentShaderSampleInterlock=interlock.fragmentShaderShadingRateInterlock=VK_FALSE;
                    features.fragmentStoresAndAtomics=VK_TRUE;
                    dc.pNext=&interlock;
                    device_extensions.push_back(VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME);
                }
            }
        }
        dc.enabledExtensionCount=unsigned(device_extensions.size()); dc.ppEnabledExtensionNames=device_extensions.data();
        // LLVM resource-name lowering emits byte constants even when the
        // arithmetic is 32-bit. Enable this optional capability at device creation.
        VkPhysicalDeviceShaderFloat16Int8Features integers{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};
        if (api_version>=VK_API_VERSION_1_3) {
            VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            query.pNext=&integers; vkGetPhysicalDeviceFeatures2(physical,&query);
            shader_int8=integers.shaderInt8;
            integers.shaderFloat16=VK_FALSE;
            integers.pNext=const_cast<void*>(dc.pNext); dc.pNext=&integers;
        }
#ifdef ANYPS2_LLVM_TEXTURES
        if (!shader_int8) throw std::runtime_error("LLVM texture kernels require Vulkan shaderInt8");
#endif
        check(vkCreateDevice(physical,&dc,nullptr,&device),"Create device"); vkGetDeviceQueue(device,family,0,&queue);
        if (path==RenderPath::native_images) {
            native=std::make_unique<NativeRaster>(physical,device,queue,family,ordered_fragments);
            if (surface) create_swapchain();
            return;
        }
#if ANYPS2_REFERENCE_RENDERER
        constexpr auto transfers=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        create_buffer(transfers|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,0,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,storage,storage_allocation);
        // GPU working storage is separate from CPU staging. Cached coherent
        // host memory makes full-store readback a normal native memory copy.
        create_buffer(transfers,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      VK_MEMORY_PROPERTY_HOST_CACHED_BIT,buffer,allocation);
        check(vkMapMemory(device,allocation,0,buffer_bytes,0,&mapped),"Map graphics staging buffer");
        VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; sl.bindingCount=1; sl.pBindings=&binding;
        check(vkCreateDescriptorSetLayout(device,&sl,nullptr,&set_layout),"Create descriptor layout");
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dp.maxSets=1; dp.poolSizeCount=1; dp.pPoolSizes=&ps;
        check(vkCreateDescriptorPool(device,&dp,nullptr,&descriptors),"Create descriptors");
        VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; da.descriptorPool=descriptors; da.descriptorSetCount=1; da.pSetLayouts=&set_layout;
        check(vkAllocateDescriptorSets(device,&da,&set),"Allocate descriptors");
        VkDescriptorBufferInfo bi{storage,0,buffer_bytes};
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
        sm.codeSize=sizeof(scanout_code); sm.pCode=scanout_code;
        check(vkCreateShaderModule(device,&sm,nullptr,&module),"Create display shader");
        cp.stage.module=module;
        const auto display_result=vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&cp,nullptr,&display_pipeline);
        vkDestroyShaderModule(device,module,nullptr); check(display_result,"Create display pipeline");
        VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pc.queueFamilyIndex=family; pc.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        check(vkCreateCommandPool(device,&pc,nullptr,&pool),"Create command pool");
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool=pool; ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount=1;
        check(vkAllocateCommandBuffers(device,&ca,&command),"Allocate command buffer");
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; check(vkCreateFence(device,&fi,nullptr,&fence),"Create fence");
        if (surface) create_swapchain();
#endif
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
        sc.imageFormat=format; sc.imageColorSpace=selected->colorSpace; sc.imageExtent=extent; sc.imageArrayLayers=1; sc.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|(native ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0);
        sc.preTransform=caps.currentTransform; sc.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR; sc.presentMode=VK_PRESENT_MODE_FIFO_KHR; sc.clipped=VK_TRUE;
        if (native) {
            // Application display deadlines already pace converted execution.
            // Do not add a FIFO of stale fields paced by the desktop monitor.
            unsigned mode_count=0;
            check(vkGetPhysicalDeviceSurfacePresentModesKHR(physical,surface,&mode_count,nullptr),"Query presentation modes");
            std::vector<VkPresentModeKHR> modes(mode_count);
            check(vkGetPhysicalDeviceSurfacePresentModesKHR(physical,surface,&mode_count,modes.data()),"Read presentation modes");
            if (std::find(modes.begin(),modes.end(),VK_PRESENT_MODE_MAILBOX_KHR)!=modes.end())
                sc.presentMode=VK_PRESENT_MODE_MAILBOX_KHR;
        }
        check(vkCreateSwapchainKHR(device,&sc,nullptr,&swapchain),"Create swapchain");
        check(vkGetSwapchainImagesKHR(device,swapchain,&count,nullptr),"Get swapchain images"); images.resize(count); check(vkGetSwapchainImagesKHR(device,swapchain,&count,images.data()),"Get swapchain images");
        VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (native) {
            for (auto& frame:present_frames) check(vkCreateSemaphore(device,&sem,nullptr,&frame.acquired),"Create native acquire semaphore");
            // Reacquiring an image retires the presentation engine's previous wait.
            // A graphics fence alone cannot prove that a present semaphore is reusable.
            present_ready.resize(images.size());
            for (auto& ready:present_ready) check(vkCreateSemaphore(device,&sem,nullptr,&ready),"Create native present semaphore");
        } else {
            check(vkCreateSemaphore(device,&sem,nullptr,&acquired),"Create acquire semaphore");
            check(vkCreateSemaphore(device,&sem,nullptr,&rendered),"Create render semaphore");
        }
    }
    void resolve(const DisplayPlan& plan) {
#if ANYPS2_REFERENCE_RENDERER

        if (!initialized || !plan.width || !plan.height || plan.width>2048 || plan.height>2048 || !(plan.control&3))
            throw std::runtime_error("Invalid resident display plan");
        for (unsigned i=0;i<2;++i) if (plan.control&(1U<<i)) {
            const auto& surface=plan.surfaces[i];
            if (!surface.pitch || surface.pitch>63 || surface.base>16383 || !surface.width || !surface.height ||
                surface.offset_x>plan.width || surface.offset_y>plan.height ||
                surface.width>plan.width-surface.offset_x || surface.height>plan.height-surface.offset_y)
                throw std::runtime_error("Invalid resident display surface");
        }
        order();
        const bool reset=display_width!=plan.width || display_height!=plan.height || display_bgra!=plan.bgra;
        if (reset) vkCmdFillBuffer(command,storage,display_offset,display_bytes,0);
        vkCmdFillBuffer(command,storage,display_offset-4,4,reset ? 1 : 0);
        order();
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,display_pipeline);
        vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,nullptr);
        vkCmdPushConstants(command,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(plan),&plan);
        vkCmdDispatch(command,(plan.width+7)/8,(plan.height+7)/8,1);
        display_width=plan.width; display_height=plan.height; display_bgra=plan.bgra;

#else
    throw std::runtime_error("Reference renderer operation is unavailable in native output");
#endif
}
    void copy_to_host(VkDeviceSize offset,VkDeviceSize bytes) {
        order(); const VkBufferCopy copy{offset,0,bytes};
        vkCmdCopyBuffer(command,storage,buffer,1,&copy);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        flush(); statistics.readback_bytes+=bytes;
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
VulkanRenderer::VulkanRenderer(void* window,bool validation,RenderPath path):impl_(std::make_unique<Impl>()) {impl_->path=path; impl_->initialize(window,validation);}
VulkanRenderer::~VulkanRenderer()=default;
void VulkanRenderer::synchronize() { if (impl_->native) { impl_->native->finish(); return; } impl_->flush(); check(vkDeviceWaitIdle(impl_->device),"Synchronize graphics"); }
const std::string& VulkanRenderer::device_name() const {return impl_->name;}
std::uint64_t VulkanRenderer::enqueue_finish() {
    if (impl_->native) return impl_->native->enqueue_finish();
    synchronize(); return 0;
}
std::uint64_t VulkanRenderer::completed_submission() {
    return impl_->native ? impl_->native->completed_submission() : 0;
}
unsigned VulkanRenderer::validation_errors() const {return impl_->errors.load();}
std::string VulkanRenderer::validation_message() const {
    std::lock_guard lock(impl_->validation_mutex);
    return impl_->first_validation_error;
}
void VulkanRenderer::profile_commands(bool enable) { if (impl_->native) impl_->native->profile_commands(enable); }
const RenderStatistics& VulkanRenderer::statistics() const { return impl_->native ? impl_->native->statistics() : impl_->statistics; }
void VulkanRenderer::verify_texture_kernels(bool enable) {
    if (impl_->native) impl_->native->verify_texture_kernels(enable);
    else if (enable) throw std::runtime_error("LLVM texture verification requires the native renderer");
}
namespace {
void validate_sprite(const Sprite& draw) {
    if ((draw.flags&indexed8_flag) && !(draw.flags&64)) throw std::runtime_error("Invalid indexed texture flags");

    if (draw.flags&~(((1U<<22)-1)|texture_16_flag|texture_16s_flag|line_flag|indexed8_flag|fog_flag|color_16_flag|color_16s_flag|color_z_flag|texture_z_flag|texture_high_flag)) throw std::runtime_error("Unknown Vulkan draw flags");
    if ((draw.flags&color_16s_flag) && !(draw.flags&color_16_flag)) throw std::runtime_error("Invalid 16-bit surface flags");
    if ((draw.flags&color_16_flag) && draw.right>draw.pitch*64) throw std::runtime_error("Wrapped 16-bit render target requires explicit mapping");
    if ((draw.flags&line_flag) && (draw.flags&(1|triangle_flag|perspective_triangle_flag)))
        throw std::runtime_error("Unsupported native line attributes");
    if (draw.right>2048 || draw.bottom>2048 || draw.left>draw.right || draw.top>draw.bottom || !draw.pitch || draw.pitch>63 || draw.base>16383)
        throw std::runtime_error("Invalid Vulkan sprite bounds or GS memory");
    if (draw.left==draw.right || draw.top==draw.bottom) return;
    if ((draw.flags&(256|depth_test_flag)) || (draw.test&0x4000)) {
        if ((draw.right>draw.pitch*64 && !can_map_wrapped_sprite(draw)) ||
            ((draw.bottom-1)/32-draw.top/32)*draw.pitch+(draw.right-1)/64-draw.left/64>=512)
            throw std::runtime_error("Destination-dependent sprite requires non-aliased framebuffer");
        if (draw.flags&256) for (unsigned shift=0;shift<8;shift+=2)
            if (((draw.blend>>shift)&3)>2) throw std::runtime_error("Unsupported Vulkan blend selector");
    }
    if (draw.flags&depth_test_flag) {
        if (draw.depth_base>16352 || draw.depth_base%32)
            throw std::runtime_error("Invalid native depth surface");
        if (draw.mask!=0xffffffffU && ((draw.test>>17)&3)!=0 &&
            ((draw.flags&depth_write_flag) || ((draw.test>>17)&3)>=2)) {
            const auto color_page_height=(draw.flags&color_16_flag) ? 64U : 32U;
            const auto overlap=[](auto&& color_pages,auto&& depth_pages) {
                std::array<bool,512> pages{};
                color_pages([&](unsigned page) { pages[page]=true; });
                bool found=false;
                depth_pages([&](unsigned page) { found|=pages[page]; });
                return found;
            };
            const auto pages_of=[&](unsigned base,unsigned height) {
                return [&draw,base,height](auto&& visit) {
                    for (unsigned y=draw.top/height;y<=(draw.bottom-1)/height;++y)
                        for (unsigned x=draw.left/64;x<=(draw.right-1)/64;++x)
                            visit((base/32+y*draw.pitch+x)%512);
                };
            };
            const auto color_pages=pages_of(draw.base,color_page_height),depth_pages=pages_of(draw.depth_base,32);
            // Primitives inside one color page and one depth page compare directly.
            const bool single=draw.top/color_page_height==(draw.bottom-1)/color_page_height &&
                draw.top/32==(draw.bottom-1)/32 && draw.left/64==(draw.right-1)/64;
            const bool overlapping=single ?
                (draw.base/32+(draw.top/color_page_height)*draw.pitch+draw.left/64)%512==
                (draw.depth_base/32+(draw.top/32)*draw.pitch+draw.left/64)%512 :
                overlap(color_pages,depth_pages);
            if (overlapping) throw std::runtime_error("Overlapping native color and depth surfaces");
        }
    }
    if ((draw.flags&1) && ((!(draw.flags&triangle_flag) && (!draw.dx || !draw.dy)) || !draw.texture_pitch || !draw.texture_width ||
        !draw.texture_height || draw.right>draw.pitch*64 || ((draw.bottom-1)/32-draw.top/32)*draw.pitch+(draw.right-1)/64-draw.left/64>=512))
        throw std::runtime_error("Unsupported textured sprite dimensions or aliased framebuffer");
    if (draw.flags&1) {
        const auto power_of_two=[](unsigned n){return n && !(n&(n-1)) && n<=1024;};
        const auto coordinate=[](int n){return n>=-65535 && n<=65535;};
        const auto delta=[](int n){return n>=-16383 && n<=16383;};
        if (draw.texture_base>16383 || draw.texture_pitch>63 || !power_of_two(draw.texture_width) ||
            !power_of_two(draw.texture_height) || draw.clamp_u>3 || draw.clamp_v>3 ||
            draw.min_u>1023 || draw.max_u>1023 || draw.min_v>1023 || draw.max_v>1023 ||
            (draw.clamp_u==2 && draw.min_u>draw.max_u) || (draw.clamp_v==2 && draw.min_v>draw.max_v) ||
            !coordinate(draw.x0) || !coordinate(draw.y0) || !coordinate(draw.dx) || !coordinate(draw.dy) ||
            draw.u0<0 || draw.u0>16383 || draw.v0<0 || draw.v0>16383 || !delta(draw.du) || !delta(draw.dv))
            throw std::runtime_error("Unsupported Vulkan texture parameters");
    }
}
}
void VulkanRenderer::sprite(std::span<std::uint32_t> memory,const Sprite& draw,std::span<const std::uint32_t> palette) {
    if ((draw.flags&64) && palette.size()!=((draw.flags&indexed8_flag) ? 256U : 16U) &&
        !(!(draw.flags&indexed8_flag) && palette.size()==256))
        throw std::runtime_error("Indexed texture has the wrong palette size");
    SpriteCommand command{draw};
    if (draw.flags&64) std::copy(palette.begin(),palette.end(),command.palette.begin());
    sprites(memory,std::span(&command,1));
}
void VulkanRenderer::validate(std::span<const SpriteCommand> commands) {
    if (commands.size()>65536) throw std::runtime_error("Vulkan draw batch exceeds bound");
    for (const auto& command:commands) validate(command.draw,command.triangle,command.fog_color,command.fog,command.levels);
}
void VulkanRenderer::validate_target(const Sprite& draw) { validate_sprite(draw); }
void VulkanRenderer::validate(const Sprite& draw,const std::array<RasterVertex,3>& triangle,
        std::uint32_t fog_color,std::uint32_t fog,const TextureLevels& levels) {
    validate_sprite(draw);
    if (const auto control=levels.control) {
        const auto count=texture_lod_levels(control),mode=texture_lod_mip_mode(control);
        if (!(draw.flags&1) || !(control&texture_lod_enabled) || count>6 || mode>2 || (count!=0)!=(mode!=0) ||
            (control&0xf000fc00U) || std::any_of(levels.base.begin(),levels.base.end(),[](auto b) { return b>16383; }) ||
            std::any_of(levels.pitch.begin(),levels.pitch.end(),[](auto p) { return p>63; }))
            throw std::runtime_error("Invalid native texture level plan");
    }
    if (fog>255 || fog_color>0xffffffU ||
        std::any_of(triangle.begin(),triangle.end(),[](const RasterVertex& v) { return v.fog>255; }))
        throw std::runtime_error("Invalid native fog parameters");
    if (draw.flags&line_flag) {
        const auto& v=triangle;
        for (unsigned i=0;i<2;++i)
            if (v[i].x < -65535 || v[i].x>65535 || v[i].y < -65535 || v[i].y>65535)
                throw std::runtime_error("Invalid Vulkan line vertex");
        if (v[0].x==v[1].x && v[0].y==v[1].y)
            throw std::runtime_error("Degenerate Vulkan line");
    }
    if (draw.flags&triangle_flag) {
        for (const auto& vertex:triangle)
            if (vertex.x < -65535 || vertex.x>65535 || vertex.y < -65535 || vertex.y>65535 || vertex.u>16383 || vertex.v>16383)
                throw std::runtime_error("Invalid Vulkan triangle vertex");
        const auto& v=triangle;
        if (draw.flags&depth_test_flag) {
            const auto [left,right]=vertex_range(v[0].x,v[1].x,v[2].x);
            const auto [top,bottom]=vertex_range(v[0].y,v[1].y,v[2].y);
            if (right-left>65535 || bottom-top>65535)
                throw std::runtime_error("Native depth interpolation exceeds coordinate bound");
        }
        if (draw.flags&perspective_triangle_flag) {
            for (const auto& vertex:v) {
                if (!std::isfinite(vertex.s) || !std::isfinite(vertex.t) || !std::isfinite(vertex.q) ||
                    std::abs(vertex.q)<1e-20f || std::abs(vertex.q)>1e20f ||
                    std::abs(vertex.s)>1e20f || std::abs(vertex.t)>1e20f ||
                    std::signbit(vertex.q)!=std::signbit(v[0].q) ||
                    std::abs(double(vertex.s)/vertex.q*draw.texture_width*16)>0x3fffffff ||
                    std::abs(double(vertex.t)/vertex.q*draw.texture_height*16)>0x3fffffff) {
                    std::ostringstream detail;
                    detail<<"Unsupported native triangle ST/Q range or zero crossing; texture="
                        <<draw.texture_width<<'x'<<draw.texture_height<<" bounds=("
                        <<draw.left<<','<<draw.top<<','<<draw.right<<','<<draw.bottom<<')'
                        <<std::hexfloat;
                    for (unsigned i=0;i<v.size();++i)
                        detail<<" vertex["<<i<<"]=("<<v[i].x<<','<<v[i].y<<") STQ=("
                            <<v[i].s<<','<<v[i].t<<','<<v[i].q<<')';
                    throw std::runtime_error(detail.str());
                }
            }
        }
        const auto area=(std::int64_t(v[1].x)-v[0].x)*(std::int64_t(v[2].y)-v[0].y)-
            (std::int64_t(v[1].y)-v[0].y)*(std::int64_t(v[2].x)-v[0].x);
        if (area<=0) throw std::runtime_error("Vulkan triangle must have positive area");
    }
}
void VulkanRenderer::sprites(std::span<std::uint32_t> memory,std::span<const SpriteCommand> commands) {
    if (memory.size_bytes()!=memory_bytes) throw std::runtime_error("Invalid graphics storage");
    validate(commands);
    upload(memory); draw(commands); read(memory);
}
void VulkanRenderer::upload(std::span<const std::uint32_t> memory) {
    if (impl_->native) { impl_->native->upload(memory); return; }
#if ANYPS2_REFERENCE_RENDERER

    if (memory.size_bytes()!=memory_bytes) throw std::runtime_error("Invalid graphics upload size");
    auto& v=*impl_; v.flush();
    std::memcpy(v.mapped,memory.data(),memory_bytes);
    v.order();
    const VkBufferCopy copy{0,0,memory_bytes};
    vkCmdCopyBuffer(v.command,v.buffer,v.storage,1,&copy);
    v.initialized=true; v.statistics.upload_bytes+=memory_bytes;

#else
    throw std::runtime_error("Reference renderer operation is unavailable in native output");
#endif
}
bool VulkanRenderer::enable_vector_drawing() { return impl_->native && impl_->native->enable_vectors(); }
unsigned VulkanRenderer::load_vector_kernel(std::span<const std::uint32_t> spirv,unsigned unit) {
    if (!impl_->native) throw std::invalid_argument("Vector kernels require the native image renderer");
    if (impl_->api_version<VK_API_VERSION_1_3 || !impl_->shader_int8)
        throw std::invalid_argument("LLVM vector kernels require Vulkan 1.3 and shaderInt8");
    return impl_->native->load_vector_kernel(spirv,unit);
}
void VulkanRenderer::draw_vectors(const VectorLaunch& launch,const VectorAssembly& assembly,const SpriteCommand& command) {
    if (!impl_->native) throw std::invalid_argument("Vector batches require the native image renderer");
    impl_->native->draw_vectors(launch,assembly,command);
}
void VulkanRenderer::draw(std::span<const SpriteCommand> commands) {
    if (impl_->native) { impl_->native->draw(commands); return; }
#if ANYPS2_REFERENCE_RENDERER

    validate(commands);
    for (const auto& command:commands) if ((command.draw.flags&fog_flag) || command.levels.control)
        throw std::runtime_error("Fog and LOD texture sampling require the native Vulkan image renderer");
    // This renderer uses the same two flag bits for its own upload and direct-texture draws.
    for (const auto& command:commands) if (command.draw.flags&(texture_16_flag|texture_16s_flag))
        throw std::runtime_error("16-bit texture sampling requires the native Vulkan image renderer");
    auto& v=*impl_;
    if (!v.initialized) throw std::runtime_error("Graphics storage was not initialized");
    for (const auto& command:commands) {
        auto draw=command.draw;
        draw.flags&=~direct_texture_flag;
        if (draw.left==draw.right || draw.top==draw.bottom) continue;
        v.order(); v.bind();
        if (draw.flags&1) {
            // Snapshot immediately before this draw, including prior draws in
            // the same batch. This preserves overlapping framebuffer textures.
            // Include every texel reachable through wrap/region modes, including
            // bilinear neighbours. Copy complete storage pages so swizzling and
            // unaligned texture bases cannot leave holes in the snapshot.
            const auto source=texture_storage(draw);
            const bool color_overlap=overlaps(source,output_storage(draw,draw.base));
            const bool depth_overlap=(draw.flags&depth_test_flag) && (draw.flags&depth_write_flag) &&
                overlaps(source,output_storage(draw,draw.depth_base));
            if (!color_overlap && !depth_overlap) {
                draw.flags|=direct_texture_flag;
                ++v.statistics.direct_texture_draws;
            } else {
                const auto ranges=split_storage(source);
                const std::array<VkBufferCopy,2> copies{{
                    {ranges[0].offset,memory_bytes+ranges[0].offset,ranges[0].length},
                    {ranges[1].offset,memory_bytes+ranges[1].offset,ranges[1].length}}};
                vkCmdCopyBuffer(v.command,v.storage,v.storage,ranges[1].length ? 2 : 1,copies.data());
                v.statistics.texture_snapshot_bytes+=source.length;
            }
        }
        if (draw.flags&64) vkCmdUpdateBuffer(v.command,v.storage,memory_bytes*2,(draw.flags&indexed8_flag) ? 1024 : 64,command.palette.data());
        if (draw.flags&(triangle_flag|line_flag)) vkCmdUpdateBuffer(v.command,v.storage,triangle_offset,sizeof(command.triangle),command.triangle.data());
        if (draw.flags&(1|64|triangle_flag|line_flag)) {
            VkMemoryBarrier uploaded{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            uploaded.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
            uploaded.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(v.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0,1,&uploaded,0,nullptr,0,nullptr);
        }
        vkCmdPushConstants(v.command,v.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(draw),&draw);
        vkCmdDispatch(v.command,(draw.right-draw.left+7)/8,(draw.bottom-draw.top+7)/8,1);
        if (draw.flags&triangle_flag) ++v.statistics.triangles; else ++v.statistics.sprites;
        v.bounded();
    }

#else
    throw std::runtime_error("Reference renderer operation is unavailable in native output");
#endif
}
void VulkanRenderer::read(std::span<std::uint32_t> memory,unsigned start,unsigned length) {
    if (impl_->native) { impl_->native->read(memory,start,length); return; }
    read(memory);
}
void VulkanRenderer::read(std::span<std::uint32_t> memory) {
    if (impl_->native) { impl_->native->read(memory); return; }
#if ANYPS2_REFERENCE_RENDERER

    if (memory.size_bytes()!=memory_bytes) throw std::runtime_error("Invalid graphics readback size");
    auto& v=*impl_;
    if (!v.initialized) throw std::runtime_error("Graphics storage was not initialized");
    v.order(); const VkBufferCopy copy{0,0,memory_bytes};
    vkCmdCopyBuffer(v.command,v.storage,v.buffer,1,&copy);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(v.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    v.flush(); std::memcpy(memory.data(),v.mapped,memory_bytes);
    v.statistics.readback_bytes+=memory_bytes;

#else
    throw std::runtime_error("Reference renderer operation is unavailable in native output");
#endif
}
std::uint32_t VulkanRenderer::load_palette(std::span<const unsigned> addresses,unsigned first) {
    return impl_->native ? impl_->native->load_palette(addresses,first) : 0;
}
std::uint32_t VulkanRenderer::merge_palette(const std::array<std::uint32_t,16>& banks,const std::array<std::uint32_t,256>& current) {
    if (!impl_->native) throw std::runtime_error("GPU palettes require the native Vulkan image renderer");
    return impl_->native->merge_palette(banks,current);
}
void VulkanRenderer::release_palette(std::uint32_t slot) {
    if (impl_->native) impl_->native->release_palette(slot);
}
void VulkanRenderer::read_palette(std::uint32_t slot,std::span<std::uint32_t,256> words) {
    if (!impl_->native) throw std::logic_error("GPU palettes require the native renderer");
    impl_->native->read_palette(slot,words);
}
void VulkanRenderer::read_words(std::span<const unsigned> addresses,std::span<std::uint32_t> values) {
    if (impl_->native) { impl_->native->read_words(addresses,values); return; }
#if ANYPS2_REFERENCE_RENDERER

    if (addresses.size()!=values.size() || addresses.size()>256) throw std::runtime_error("Invalid graphics word readback");
    for (auto address:addresses) if (address>=memory_bytes/4) throw std::runtime_error("Graphics readback address out of range");
    if (addresses.empty()) return;
    auto& v=*impl_;
    if (!v.initialized) throw std::runtime_error("Graphics storage was not initialized");
    v.order();
    std::vector<VkBufferCopy> copies; copies.reserve(addresses.size());
    for (std::size_t i=0;i<addresses.size();++i) copies.push_back({VkDeviceSize(addresses[i])*4,i*4,4});
    vkCmdCopyBuffer(v.command,v.storage,v.buffer,unsigned(copies.size()),copies.data());
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(v.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    v.flush(); std::memcpy(values.data(),v.mapped,values.size_bytes());
    v.statistics.readback_bytes+=values.size_bytes();

#else
    throw std::runtime_error("Reference renderer operation is unavailable in native output");
#endif
}
void VulkanRenderer::set_dither(std::uint64_t matrix) {
    if (impl_->native) impl_->native->set_dither(matrix);
}
void VulkanRenderer::write(const ImageRect& rect) {
    if (impl_->native) { impl_->native->write(rect); return; }
    const auto writes=rect.writes();
    write(writes);
}
void VulkanRenderer::write(std::span<const PixelWrite> writes) {
    if (impl_->native) { impl_->native->write(writes); return; }
#if ANYPS2_REFERENCE_RENDERER

    for (const auto& write:writes) if (write.address>=memory_bytes/4 || (write.value&~write.mask))
        throw std::runtime_error("Invalid native graphics write");
    if (writes.empty()) return;
    auto& v=*impl_;
    if (!v.initialized) throw std::runtime_error("Graphics storage was not initialized");
    // Merge repeated word/nibble writes in original order before parallel GPU
    // application. Distinct invocations then have disjoint destinations.
    std::vector<PixelWrite> merged(writes.begin(),writes.end());
    std::stable_sort(merged.begin(),merged.end(),[](auto a,auto b){return a.address<b.address;});
    std::size_t count=0;
    for (const auto write:merged) {
        if (count && merged[count-1].address==write.address) {
            auto& previous=merged[count-1];
            previous.value=(previous.value&~write.mask)|write.value; previous.mask|=write.mask;
        } else merged[count++]=write;
    }
    merged.resize(count);
    for (std::size_t first=0;first<count;first+=max_writes) {
        const auto n=std::min<std::size_t>(max_writes,count-first);
        v.order(); v.bind();
        const auto* data=reinterpret_cast<const std::uint8_t*>(merged.data()+first);
        for (VkDeviceSize offset=0;offset<n*sizeof(PixelWrite);) {
            const auto bytes=std::min<VkDeviceSize>(65536,n*sizeof(PixelWrite)-offset);
            vkCmdUpdateBuffer(v.command,v.storage,writes_offset+offset,bytes,data+offset); offset+=bytes;
        }
        v.order();
        Sprite upload{}; upload.flags=1U<<23; upload.right=unsigned(n);
        vkCmdPushConstants(v.command,v.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(upload),&upload);
        vkCmdDispatch(v.command,unsigned((n+63)/64),1,1);
        v.statistics.upload_bytes+=n*sizeof(PixelWrite); v.bounded();
    }

#else
    throw std::runtime_error("Reference renderer operation is unavailable in native output");
#endif
}
Scanout VulkanRenderer::scanout_readback(DisplayPlan plan) {
    if (impl_->native) return impl_->native->capture(plan);
#if ANYPS2_REFERENCE_RENDERER
    auto& v=*impl_; plan.bgra=0; v.resolve(plan);
    Scanout frame{plan.width,plan.height,std::vector<std::uint32_t>(std::size_t(plan.width)*plan.height)};
    v.copy_to_host(display_offset,frame.pixels.size()*4);
    std::memcpy(frame.pixels.data(),v.mapped,frame.pixels.size()*4); return frame;
#else
    throw std::runtime_error("Reference scanout unavailable");
#endif
}
void VulkanRenderer::refresh_surface() {
    auto& v=*impl_;
    if (!v.surface) throw std::runtime_error("Cannot resize a headless renderer");
    if (v.native) v.native->finish(); else v.flush();
    check(vkDeviceWaitIdle(v.device),"Retire presentation before resize");
    if (v.native) v.native->release_presentation();
    for (auto& frame:v.present_frames) {
        if (frame.acquired) vkDestroySemaphore(v.device,frame.acquired,nullptr);
        frame={};
    }
    for (auto ready:v.present_ready) vkDestroySemaphore(v.device,ready,nullptr);
    v.present_ready.clear(); v.present_frame=0;
    if (v.acquired) vkDestroySemaphore(v.device,v.acquired,nullptr);
    if (v.rendered) vkDestroySemaphore(v.device,v.rendered,nullptr);
    v.acquired={}; v.rendered={};
    v.images.clear();
    if (v.swapchain) vkDestroySwapchainKHR(v.device,v.swapchain,nullptr);
    v.swapchain={};
    v.create_swapchain();
}
bool VulkanRenderer::present_display(DisplayPlan plan) {
    auto& v=*impl_;
    if (v.native) {
        if (!v.swapchain) throw std::runtime_error("Presentation requires a window");
        // The surface may report another size while the window is being changed.
        if (plan.presented_width()!=v.extent.width || plan.presented_height()!=v.extent.height) return false;
        if (!v.native->changed(plan)) return false;
        auto& frame=v.present_frames[v.present_frame];
        v.native->wait_submission(frame.submission);
        // A window change (move between monitors, resize, display mode) makes
        // the swapchain out of date; recreate it and show the next image.
        unsigned index=0;
        const auto acquired=vkAcquireNextImageKHR(v.device,v.swapchain,UINT64_MAX,frame.acquired,VK_NULL_HANDLE,&index);
        if (acquired==VK_ERROR_OUT_OF_DATE_KHR) { refresh_surface(); return false; }
        if (acquired!=VK_SUBOPTIMAL_KHR) check(acquired,"Acquire native image");
        frame.submission=v.native->present(plan,v.images[index],v.format,frame.acquired,v.present_ready[index]);
        VkPresentInfoKHR p{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; p.waitSemaphoreCount=1; p.pWaitSemaphores=&v.present_ready[index]; p.swapchainCount=1; p.pSwapchains=&v.swapchain; p.pImageIndices=&index;
        const auto presented=vkQueuePresentKHR(v.queue,&p);
        v.present_frame=(v.present_frame+1)%v.present_frames.size();
        if (presented==VK_ERROR_OUT_OF_DATE_KHR || presented==VK_SUBOPTIMAL_KHR || acquired==VK_SUBOPTIMAL_KHR) refresh_surface();
        else check(presented,"Present native image");
        return true;
    }
#if ANYPS2_REFERENCE_RENDERER
    if (!v.initialized) return false;
    if (!v.swapchain || plan.width!=v.extent.width || plan.height!=v.extent.height)
        throw std::runtime_error("Presentation size must match the fixed window");
    plan.bgra=v.format==VK_FORMAT_B8G8R8A8_UNORM; v.resolve(plan);
    v.copy_to_host(display_offset-4,4);
    if (!*static_cast<const std::uint32_t*>(v.mapped)) return false;
    present_buffer(true,plan.width,plan.height); return true;
#else
    throw std::runtime_error("Reference scanout unavailable");
#endif
}
void VulkanRenderer::present(std::span<const std::uint32_t> rgba,unsigned width,unsigned height) {
    auto& v=*impl_;
    if (v.native) throw std::runtime_error("Native image renderer does not accept CPU presentation images");
    v.flush();
    if (rgba.size()!=std::size_t(width)*height || rgba.size_bytes()>display_bytes)
        throw std::runtime_error("Invalid presentation pixels");
    auto* output=static_cast<std::uint32_t*>(v.mapped);
    for (std::size_t i=0;i<rgba.size();++i) {auto p=rgba[i];output[i]=v.format==VK_FORMAT_B8G8R8A8_UNORM ? (p&0xff00ff00U)|((p&255)<<16)|((p>>16)&255) : p;}
    present_buffer(false,width,height);
}
void VulkanRenderer::present_buffer(bool resident,unsigned width,unsigned height) {
#if ANYPS2_REFERENCE_RENDERER

    auto& v=*impl_; v.flush();
    if (!v.swapchain || width!=v.extent.width || height!=v.extent.height)
        throw std::runtime_error("Presentation size must match the fixed window");
    unsigned index=0; check(vkAcquireNextImageKHR(v.device,v.swapchain,UINT64_MAX,v.acquired,VK_NULL_HANDLE,&index),"Acquire image");
    v.begin();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; b.image=v.images[index]; b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdPipelineBarrier(v.command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
    VkBufferImageCopy copy{}; copy.bufferOffset=resident ? display_offset : 0; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={width,height,1};
    vkCmdCopyBufferToImage(v.command,resident ? v.storage : v.buffer,v.images[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=0;
    vkCmdPipelineBarrier(v.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&b);
    v.submit(true);
    VkPresentInfoKHR p{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};p.waitSemaphoreCount=1;p.pWaitSemaphores=&v.rendered;p.swapchainCount=1;p.pSwapchains=&v.swapchain;p.pImageIndices=&index;
    check(vkQueuePresentKHR(v.queue,&p),"Present image"); check(vkQueueWaitIdle(v.queue),"Wait for presentation");

#else
    throw std::runtime_error("Reference renderer operation is unavailable in native output");
#endif
}
}
