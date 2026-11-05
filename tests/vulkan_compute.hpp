#pragma once
// Test-only Vulkan compute runner for AOT LLVM shaders: one dispatch over
// host-visible storage buffers, with validation errors failing the test.
#include <vulkan/vulkan.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace vulkan_compute {
inline void check(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
inline void vk_check(VkResult result,const char* message) { if (result!=VK_SUCCESS) throw std::runtime_error(message); }
struct Buffer { VkBuffer buffer{}; VkDeviceMemory memory{}; void* mapping=nullptr; };
struct Device {
    VkInstance instance{}; VkDevice device{}; VkPhysicalDevice physical{};
    VkQueue queue{}; unsigned family=0;
    VkDescriptorSetLayout descriptors{}; VkDescriptorPool descriptor_pool{};
    VkPipelineLayout layout{}; VkShaderModule shader{}; VkPipeline pipeline{};
    VkCommandPool commands{}; VkFence fence{};
    VkDebugUtilsMessengerEXT messenger{};
    std::atomic<unsigned> errors=0;
    static VKAPI_ATTR VkBool32 VKAPI_CALL validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* data,void* context) {
        if (severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
            ++static_cast<Device*>(context)->errors;
            std::cerr<<data->pMessage<<'\n';
        }
        return VK_FALSE;
    }
    std::vector<Buffer> buffers;
    ~Device() {
        if (device) {
            vkDeviceWaitIdle(device);
            if (fence) vkDestroyFence(device,fence,nullptr);
            if (commands) vkDestroyCommandPool(device,commands,nullptr);
            if (pipeline) vkDestroyPipeline(device,pipeline,nullptr);
            if (shader) vkDestroyShaderModule(device,shader,nullptr);
            if (layout) vkDestroyPipelineLayout(device,layout,nullptr);
            if (descriptor_pool) vkDestroyDescriptorPool(device,descriptor_pool,nullptr);
            if (descriptors) vkDestroyDescriptorSetLayout(device,descriptors,nullptr);
            for (auto b:buffers) {
                if (b.mapping) vkUnmapMemory(device,b.memory);
                if (b.buffer) vkDestroyBuffer(device,b.buffer,nullptr);
                if (b.memory) vkFreeMemory(device,b.memory,nullptr);
            }
            vkDestroyDevice(device,nullptr);
        }
        if (messenger) reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT"))(instance,messenger,nullptr);
        if (instance) vkDestroyInstance(instance,nullptr);
    }
    void initialize() {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion=VK_API_VERSION_1_3;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ci.pApplicationInfo=&app;
        const char* layer="VK_LAYER_KHRONOS_validation";
        const char* extension=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        ci.enabledLayerCount=1; ci.ppEnabledLayerNames=&layer;
        ci.enabledExtensionCount=1; ci.ppEnabledExtensionNames=&extension;
        vk_check(vkCreateInstance(&ci,nullptr,&instance),"Create kernel-test instance");
        VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        debug.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debug.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
        debug.pfnUserCallback=validation; debug.pUserData=this;
        vk_check(reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,
            "vkCreateDebugUtilsMessengerEXT"))(instance,&debug,nullptr,&messenger),"Create validation messenger");
        unsigned count=0; vk_check(vkEnumeratePhysicalDevices(instance,&count,nullptr),"Enumerate devices");
        std::vector<VkPhysicalDevice> devices(count);
        vk_check(vkEnumeratePhysicalDevices(instance,&count,devices.data()),"Enumerate devices");
        int best=-1;
        for (auto candidate:devices) {
            VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(candidate,&properties);
            VkPhysicalDeviceFeatures features{}; vkGetPhysicalDeviceFeatures(candidate,&features);
            if (properties.apiVersion<VK_API_VERSION_1_3 || !features.shaderInt64) continue;
            unsigned families=0; vkGetPhysicalDeviceQueueFamilyProperties(candidate,&families,nullptr);
            std::vector<VkQueueFamilyProperties> queues(families);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate,&families,queues.data());
            for (unsigned n=0;n<families;++n) if (queues[n].queueFlags&VK_QUEUE_COMPUTE_BIT) {
                const int score=properties.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
                if (score>best) { best=score; physical=candidate; family=n; }
                break;
            }
        }
        check(physical!=VK_NULL_HANDLE,"No Vulkan 1.3 compute device with 64-bit integers");
        VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(physical,&properties);
        std::cout<<"device="<<properties.deviceName<<'\n';
        const float priority=1;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex=family; qi.queueCount=1; qi.pQueuePriorities=&priority;
        // Converted VU integer registers are 16-bit values in generated kernels.
        VkPhysicalDeviceFeatures supported{}; vkGetPhysicalDeviceFeatures(physical,&supported);
        VkPhysicalDeviceFeatures features{}; features.shaderInt64=VK_TRUE; features.shaderInt16=supported.shaderInt16;
        VkPhysicalDeviceShaderFloat16Int8Features integers{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES};
        VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        query.pNext=&integers; vkGetPhysicalDeviceFeatures2(physical,&query);
        check(integers.shaderInt8,"LLVM resource names require shaderInt8");
        integers.shaderFloat16=VK_FALSE;
        VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; di.pNext=&integers;
        di.queueCreateInfoCount=1; di.pQueueCreateInfos=&qi; di.pEnabledFeatures=&features;
        vk_check(vkCreateDevice(physical,&di,nullptr,&device),"Create compute device");
        vkGetDeviceQueue(device,family,0,&queue);
    }
    Buffer buffer(std::size_t bytes) {
        buffers.push_back({}); auto& result=buffers.back();
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size=bytes; bi.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        vk_check(vkCreateBuffer(device,&bi,nullptr,&result.buffer),"Create kernel buffer");
        VkMemoryRequirements requirements{}; vkGetBufferMemoryRequirements(device,result.buffer,&requirements);
        VkPhysicalDeviceMemoryProperties properties{}; vkGetPhysicalDeviceMemoryProperties(physical,&properties);
        unsigned type=properties.memoryTypeCount;
        const auto wanted=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (unsigned n=0;n<properties.memoryTypeCount;++n)
            if ((requirements.memoryTypeBits&(1U<<n)) && (properties.memoryTypes[n].propertyFlags&wanted)==wanted) {type=n; break;}
        check(type<properties.memoryTypeCount,"No mapped kernel memory");
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize=requirements.size; ai.memoryTypeIndex=type;
        vk_check(vkAllocateMemory(device,&ai,nullptr,&result.memory),"Allocate kernel memory");
        vk_check(vkBindBufferMemory(device,result.buffer,result.memory,0),"Bind kernel memory");
        vk_check(vkMapMemory(device,result.memory,0,bytes,0,&result.mapping),"Map kernel memory");
        return result;
    }
    // Binds each span as storage buffer n, dispatches the given workgroups and
    // copies every buffer back, so callers can also check read-only buffers.
    void run(std::span<const std::uint32_t> code,std::span<const std::span<std::uint32_t>> data,unsigned groups) {
        check(!data.empty() && data.size()<=8 && !pipeline,"Compute runner takes one dispatch of 1-8 buffers");
        const auto count=static_cast<unsigned>(data.size());
        std::vector<Buffer> bound;
        for (auto words:data) {
            bound.push_back(buffer(words.size_bytes()));
            std::memcpy(bound.back().mapping,words.data(),words.size_bytes());
        }
        std::vector<VkDescriptorSetLayoutBinding> bindings(count);
        for (unsigned n=0;n<count;++n) bindings[n]={n,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount=count; li.pBindings=bindings.data();
        vk_check(vkCreateDescriptorSetLayout(device,&li,nullptr,&descriptors),"Create kernel descriptors");
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,count};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets=1; pi.poolSizeCount=1; pi.pPoolSizes=&size;
        vk_check(vkCreateDescriptorPool(device,&pi,nullptr,&descriptor_pool),"Create descriptor pool");
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool=descriptor_pool; allocate.descriptorSetCount=1; allocate.pSetLayouts=&descriptors;
        VkDescriptorSet set{}; vk_check(vkAllocateDescriptorSets(device,&allocate,&set),"Allocate kernel descriptors");
        std::vector<VkDescriptorBufferInfo> infos(count);
        std::vector<VkWriteDescriptorSet> writes(count);
        for (unsigned n=0;n<count;++n) {
            infos[n]={bound[n].buffer,0,data[n].size_bytes()};
            writes[n]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; writes[n].dstSet=set; writes[n].dstBinding=n;
            writes[n].descriptorCount=1; writes[n].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[n].pBufferInfo=&infos[n];
        }
        vkUpdateDescriptorSets(device,count,writes.data(),0,nullptr);
        VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout_info.setLayoutCount=1; layout_info.pSetLayouts=&descriptors;
        vk_check(vkCreatePipelineLayout(device,&layout_info,nullptr,&layout),"Create kernel pipeline layout");
        VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; si.codeSize=code.size_bytes(); si.pCode=code.data();
        vk_check(vkCreateShaderModule(device,&si,nullptr,&shader),"Create AOT shader module");
        VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeline_info.layout=layout; pipeline_info.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        pipeline_info.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; pipeline_info.stage.module=shader; pipeline_info.stage.pName="main";
        vk_check(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pipeline_info,nullptr,&pipeline),"Create AOT compute pipeline");
        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pool_info.queueFamilyIndex=family;
        vk_check(vkCreateCommandPool(device,&pool_info,nullptr,&commands),"Create command pool");
        VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        command_info.commandPool=commands; command_info.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; command_info.commandBufferCount=1;
        VkCommandBuffer command{}; vk_check(vkAllocateCommandBuffers(device,&command_info,&command),"Allocate commands");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vk_check(vkBeginCommandBuffer(command,&begin),"Begin kernel commands");
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
        vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,nullptr);
        vkCmdDispatch(command,groups,1,1);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        vk_check(vkEndCommandBuffer(command),"End kernel commands");
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; vk_check(vkCreateFence(device,&fi,nullptr,&fence),"Create kernel fence");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&command;
        vk_check(vkQueueSubmit(queue,1,&submit,fence),"Submit AOT kernel");
        vk_check(vkWaitForFences(device,1,&fence,VK_TRUE,30000000000ULL),"Wait for AOT kernel");
        for (unsigned n=0;n<count;++n) std::memcpy(data[n].data(),bound[n].mapping,data[n].size_bytes());
        check(errors.load()==0,"AOT kernel caused Vulkan validation errors");
    }
};
// Reads a SPIR-V module produced by the build.
inline std::vector<std::uint32_t> read_shader(const char* path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    const auto bytes=file.tellg(); check(bytes>0 && bytes<4*1024*1024 && bytes%4==0,"Invalid compiled shader length");
    std::vector<std::uint32_t> shader(static_cast<std::size_t>(bytes)/4);
    file.seekg(0); file.read(reinterpret_cast<char*>(shader.data()),bytes); check(bool(file),"Read shader");
    return shader;
}
}
