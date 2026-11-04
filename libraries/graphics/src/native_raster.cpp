#include "native_raster.hpp"
#include "raster_hazards.hpp"
#include "sprite_mapping.hpp"
#include <algorithm>
#include <bitset>
#include <chrono>
#include <cstring>
#include <map>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace nt::graphics {
namespace {
void checked(VkResult r,const char* operation) {
    if (r!=VK_SUCCESS) throw std::runtime_error(std::string(operation)+": Vulkan result "+std::to_string(r));
}
struct Measure {
    std::uint64_t& total;
    bool enabled;
    std::chrono::steady_clock::time_point start;
    Measure(std::uint64_t& total,bool enabled):total(total),enabled(enabled),
        start(enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}) {}
    ~Measure() { if (enabled) total+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count(); }
};
constexpr unsigned store_words=1024*1024;
constexpr VkDeviceSize staging_bytes=64*1024*1024;
constexpr unsigned frame_slots=3;
const std::uint32_t vertex_code[]=
#include "native.vert.inc"
;
const std::uint32_t fragment_code[]=
#include "native.frag.inc"
;
const std::uint32_t interlock_code[]=
#include "native-interlock.frag.inc"
;
const std::uint32_t present_vertex[]=
#include "present.vert.inc"
;
const std::uint32_t present_fragment[]=
#include "present.frag.inc"
;
const std::uint32_t field_code[]=
#include "field.comp.inc"
;
#ifdef ANYPS2_LLVM_TEXTURES
const std::uint32_t texture_decode_code[]=
#include "texture-decode.inc"
;
constexpr unsigned decode_sets_per_slot=64;
#endif
struct DrawRecord { Sprite draw; std::array<RasterVertex,3> vertices; unsigned width,height,fog_color,fog; };
static_assert(sizeof(DrawRecord)==264);
constexpr unsigned records_per_slot=4096;
constexpr VkDeviceSize record_bytes=sizeof(DrawRecord)*records_per_slot*frame_slots;
struct Region {
    unsigned start=0,length=0;
    bool contains(unsigned word) const { return ((word-start)&(store_words-1))<length; }
    bool overlaps(Region other) const { return length && other.length && (contains(other.start)||other.contains(start)); }
};
Region region(unsigned base,unsigned pitch,unsigned width,unsigned height,unsigned indexed=0) {
    const unsigned page=indexed ? ((height-1)/(indexed==2 ? 64 : 128))*(pitch/2)+(width-1)/128 : ((height-1)/32)*pitch+(width-1)/64;
    return {base*64,std::min(store_words,(page+1)*2048)};
}
unsigned texture_extent(unsigned size,unsigned mode,unsigned lo,unsigned hi) {
    return mode==2 ? hi+1 : mode==3 ? (lo|hi)+1 : size;
}
}
struct NativeRaster::Impl {
    VkPhysicalDevice physical; VkDevice device; VkQueue queue;
    bool ordered_fragments=false;
    VkQueryPool timestamps{}; double timestamp_period=0; unsigned timestamp_bits=0;
    struct Submission { VkCommandBuffer command{}; VkFence fence{}; std::uint64_t serial=0; bool measured=false; };
    std::array<Submission,frame_slots> slots{};
    unsigned slot=0; std::uint64_t submitted=0,completed=0;
    VkCommandPool pool{}; VkCommandBuffer command{};
    VkBuffer staging{}; VkDeviceMemory staging_memory{}; void* mapped=nullptr;
    VkBuffer records{}; VkDeviceMemory record_memory{}; void* record_mapping=nullptr;
    unsigned records_used=0,batch_first=0,batch_count=0,batch_vertices=0;
    VkPipeline batch_pipeline{},bound_pipeline{};
    VkDescriptorSet batch_set{},bound_set{};
    VkRenderPass render_pass{},present_pass{}; VkFormat present_format{};
    VkDescriptorSetLayout descriptors{},present_descriptors{}; VkDescriptorPool descriptor_pool{};
    VkPipelineLayout layout{},present_layout{}; VkPipeline pipeline{},coverage_pipeline{},present_pipeline{};
    VkDescriptorSetLayout field_descriptors{};
    VkPipelineLayout field_layout{}; VkPipeline field_pipeline{};
#ifdef ANYPS2_LLVM_TEXTURES
    VkDescriptorSetLayout decode_descriptors{}; VkDescriptorPool decode_pool{};
    VkPipelineLayout decode_layout{}; VkPipeline decode_pipeline{};
    VkBuffer decoded{}; VkDeviceMemory decoded_memory{};
    std::array<VkDescriptorSet,frame_slots*decode_sets_per_slot> decode_sets{};
    unsigned decodes_used=0; VkDeviceSize decode_alignment=1;
    bool verify_decodes=false;
#endif
    VkSampler sampler{};
    bool recording=false,in_pass=false,initialized=false;
    VkDeviceSize staging_used=0;
    std::uint64_t revision=1,last_present=0;
    RenderStatistics stats;
    bool profiling=false;
    std::vector<std::uint32_t> source=std::vector<std::uint32_t>(store_words);
    VkBuffer readback{};
    VkDeviceMemory readback_memory{};
    void* readback_mapping=nullptr;
    struct Image {
        VkDevice device; VkImage image{}; VkDeviceMemory memory{}; VkImageView view{};
        unsigned width,height,base=0,pitch=0,format=0; bool depth=false,dirty=false,valid=true,scratch=false;
        std::uint64_t revision=0,last_use=0,scope=0; Region region{};
        std::array<std::uint64_t,512> pages{};
        VkDeviceSize allocation_bytes=0;
        Image(VkDevice d,unsigned w,unsigned h):device(d),width(w),height(h) {}
        ~Image() {
            if (view) vkDestroyImageView(device,view,nullptr);
            if (image) vkDestroyImage(device,image,nullptr);
            if (memory) vkFreeMemory(device,memory,nullptr);
        }
    };
    std::map<std::tuple<unsigned,unsigned,unsigned>,std::unique_ptr<Image>> surfaces;
    std::uint64_t surface_use=0,surface_scope=1;
    struct Texture {
        std::unique_ptr<Image> image; Region region; bool valid=false;
        std::uint64_t last_use=0,source_revision=0;
    };
    VkDeviceSize texture_bytes=0;
    std::uint64_t texture_use=0;
    std::array<std::uint64_t,512> source_revisions{};
    std::array<std::uint64_t,512> backing_revisions{};
    void touch(Region region) {
        const auto count=std::min(512U,((region.start%2048)+region.length+2047)/2048);
        if (region.length) for (unsigned i=0;i<count;++i) source_revisions[(region.start/2048+i)%512]=revision;
    }
    std::uint64_t source_revision(Region region) const {
        std::uint64_t latest=0;
        const auto count=std::min(512U,((region.start%2048)+region.length+2047)/2048);
        if (region.length) for (unsigned i=0;i<count;++i) latest=std::max(latest,source_revisions[(region.start/2048+i)%512]);
        return latest;
    }
    // Fixed keys avoid a heap allocation for every primitive's texture lookup.
    std::map<std::array<std::uint32_t,261>,Texture> textures;
    std::map<std::pair<Image*,Image*>,VkFramebuffer> framebuffers;
    std::map<std::tuple<Image*,Image*,Image*>,VkDescriptorSet> sets;
    std::map<std::pair<Image*,Image*>,VkDescriptorSet> present_sets;
    std::map<VkImage,VkFramebuffer> present_framebuffers;
    std::vector<VkImageView> present_views;
    std::array<std::unique_ptr<Image>,4> fields;
    std::unique_ptr<Image> display_image;
    std::map<std::tuple<Image*,Image*,unsigned>,VkDescriptorSet> field_sets;
    unsigned field_index=0,field_count=0;
    DisplayField previous_field=DisplayField::progressive;
    DisplayPlan resolved_plan{};
    std::uint64_t resolved_revision=0;
    std::array<std::uint64_t,2> resolved_sources{};
    RasterHazards hazards;
    VkFramebuffer current_framebuffer{};
    unsigned current_width=0,current_height=0;
    Impl(VkPhysicalDevice p,VkDevice d,VkQueue q,bool ordered):physical(p),device(d),queue(q),ordered_fragments(ordered) {}
    void initialize(unsigned family);
    ~Impl();
    VkDeviceMemory allocate(VkMemoryRequirements requirements,VkMemoryPropertyFlags flags,VkMemoryPropertyFlags preferred=0);
    void begin(); void end_pass(); void order();
    void flush_batch();
    void append_draw(VkDescriptorSet,const SpriteCommand&);
    void collect(unsigned index,bool wait);
    void prepare_slot();
    void flush(VkSemaphore acquired=VK_NULL_HANDLE,VkSemaphore rendered=VK_NULL_HANDLE,bool wait=true);
    VkPipeline make_pipeline(VkRenderPass pass,VkPipelineLayout pipeline_layout,std::span<const std::uint32_t> vs,
        std::span<const std::uint32_t> fs,unsigned colors,bool coverage);
    std::unique_ptr<Image> image(unsigned width,unsigned height);
    void upload_image(Image&,std::span<const std::uint32_t>);
    void copy_surface_pages(Image& destination,const Image& source,const std::bitset<512>& requested);
    void written(Image&,const Sprite&);
    bool current(const Image&,const Sprite&) const;
    void export_image(Image&); void synchronize_source(Region); void clear_views();
    void trim_surfaces();
    Image& surface(unsigned base,unsigned pitch,unsigned height,bool depth,bool scratch=false,bool display=false,const Sprite* draw=nullptr,unsigned format=0);
    Image& display_surface(unsigned base,unsigned pitch,unsigned height);
    Image& texture(const SpriteCommand&);
    Image& reconstruct(const DisplayPlan&);
#ifdef ANYPS2_LLVM_TEXTURES
    void decode_texture(Image&,const SpriteCommand&,Region);
    void initialize_texture_decoder();
#endif
    VkDescriptorSet allocate_set(VkDescriptorSetLayout);
    VkDescriptorSet bind_target(Image&,Image&,Image&,const Sprite&);
};
VkDeviceMemory NativeRaster::Impl::allocate(VkMemoryRequirements requirements,VkMemoryPropertyFlags flags,VkMemoryPropertyFlags preferred) {
    VkPhysicalDeviceMemoryProperties p{}; vkGetPhysicalDeviceMemoryProperties(physical,&p);
    for (const auto wanted:{flags|preferred,flags})
    for (unsigned i=0;i<p.memoryTypeCount;++i) if ((requirements.memoryTypeBits&(1U<<i)) && (p.memoryTypes[i].propertyFlags&wanted)==wanted) {
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize=requirements.size; ai.memoryTypeIndex=i;
        VkDeviceMemory m{}; checked(vkAllocateMemory(device,&ai,nullptr,&m),"Allocate native resource memory"); return m;
    }
    throw std::runtime_error("No native resource memory type");
}
void NativeRaster::Impl::begin() {
    if (recording) return;
    prepare_slot();
    checked(vkResetCommandBuffer(command,0),"Reset native commands"); VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; b.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checked(vkBeginCommandBuffer(command,&b),"Begin native commands"); recording=true;
    if (timestamps) { vkCmdResetQueryPool(command,timestamps,slot*2,2); vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,timestamps,slot*2); }
}
void NativeRaster::Impl::flush_batch() {
    if (!batch_count) return;
    if (bound_set!=batch_set) {
        vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&batch_set,0,nullptr);
        bound_set=batch_set;
    }
    if (bound_pipeline!=batch_pipeline) { vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,batch_pipeline); bound_pipeline=batch_pipeline; }
    vkCmdDraw(command,batch_vertices,batch_count,0,batch_first);
    ++stats.draw_calls;
    if (batch_count>1) stats.batched_primitives+=batch_count;
    batch_count=0;
}
void NativeRaster::Impl::append_draw(VkDescriptorSet set,const SpriteCommand& c) {
    const auto selected=(c.draw.flags&coverage_flag) && (c.draw.flags&triangle_flag) ? coverage_pipeline : pipeline;
    const unsigned vertices=(c.draw.flags&triangle_flag) ? 3 : 6;
    if (batch_count && (set!=batch_set || selected!=batch_pipeline || vertices!=batch_vertices)) flush_batch();
    const unsigned index=slot*records_per_slot+records_used++;
    const DrawRecord record{c.draw,c.triangle,current_width,current_height,c.fog_color,c.fog};
    std::memcpy(static_cast<std::byte*>(record_mapping)+index*sizeof(DrawRecord),&record,sizeof(record));
    if (!batch_count) { batch_first=index; batch_set=set; batch_pipeline=selected; batch_vertices=vertices; }
    ++batch_count;
}
void NativeRaster::Impl::end_pass() { if (in_pass) { flush_batch(); vkCmdEndRenderPass(command); in_pass=false; current_framebuffer={}; hazards.clear(); bound_set={}; bound_pipeline={}; } }
void NativeRaster::Impl::order() {
    end_pass(); begin(); VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; b.srcAccessMask=b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&b,0,nullptr,0,nullptr);
}
void NativeRaster::Impl::collect(unsigned index,bool wait) {
    auto& entry=slots[index];
    if (!entry.serial || entry.measured) return;
    if (wait) {
        Measure timing(stats.wait_ns,profiling);
        checked(vkWaitForFences(device,1,&entry.fence,VK_TRUE,UINT64_MAX),"Wait native graphics");
    } else {
        const auto status=vkGetFenceStatus(device,entry.fence);
        if (status==VK_NOT_READY) return;
        checked(status,"Poll native graphics");
    }
    completed=std::max(completed,entry.serial);
    if (timestamps) {
        std::uint64_t values[2]{};
        checked(vkGetQueryPoolResults(device,timestamps,index*2,2,sizeof(values),values,sizeof(values[0]),VK_QUERY_RESULT_64_BIT),"Read native GPU timestamps");
        const auto mask=timestamp_bits==64 ? ~std::uint64_t(0) : (std::uint64_t(1)<<timestamp_bits)-1;
        stats.gpu_ns+=static_cast<std::uint64_t>(double((values[1]-values[0])&mask)*timestamp_period);
    }
    entry.measured=true;
}
void NativeRaster::Impl::prepare_slot() {
    // A slot owns its command buffer and upload bytes until its fence completes.
    collect(slot,true); command=slots[slot].command;
#ifdef ANYPS2_LLVM_TEXTURES
    decodes_used=0;
#endif
}
void NativeRaster::Impl::flush(VkSemaphore acquired,VkSemaphore rendered,bool wait) {
    end_pass();
    if (recording) {
        auto& entry=slots[slot];
        if (timestamps) vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,timestamps,slot*2+1);
        checked(vkEndCommandBuffer(command),"End native commands"); checked(vkResetFences(device,1,&entry.fence),"Reset native fence");
        VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO}; s.commandBufferCount=1; s.pCommandBuffers=&command;
        VkPipelineStageFlags stage=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        if (acquired) { s.waitSemaphoreCount=1; s.pWaitSemaphores=&acquired; s.pWaitDstStageMask=&stage; s.signalSemaphoreCount=1; s.pSignalSemaphores=&rendered; }
        { Measure timing(stats.wait_ns,profiling); checked(vkQueueSubmit(queue,1,&s,entry.fence),"Submit native graphics"); }
        entry.serial=++submitted; entry.measured=false;
        if (wait) collect(slot,true);
        recording=false; staging_used=0; records_used=0; ++stats.submissions;
        slot=(slot+1)%frame_slots;
    }
    if (wait) for (unsigned i=0;i<frame_slots;++i) collect(i,true);
}
VkPipeline NativeRaster::Impl::make_pipeline(VkRenderPass pass,VkPipelineLayout pipeline_layout,
    std::span<const std::uint32_t> vs,std::span<const std::uint32_t> fs,unsigned colors,bool coverage) {
    VkShaderModule modules[2]{};
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; mi.codeSize=vs.size_bytes(); mi.pCode=vs.data();
    checked(vkCreateShaderModule(device,&mi,nullptr,&modules[0]),"Create native vertex shader"); mi.codeSize=fs.size_bytes(); mi.pCode=fs.data();
    const auto fragment_result=vkCreateShaderModule(device,&mi,nullptr,&modules[1]);
    if (fragment_result!=VK_SUCCESS) { vkDestroyShaderModule(device,modules[0],nullptr); checked(fragment_result,"Create native fragment shader"); }
    std::array<VkPipelineShaderStageCreateInfo,2> stages{};
    for (unsigned i=0;i<2;++i) { stages[i].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stages[i].stage=i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT; stages[i].module=modules[i]; stages[i].pName="main"; }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount=vp.scissorCount=1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; rs.polygonMode=VK_POLYGON_MODE_FILL; rs.cullMode=VK_CULL_MODE_NONE; rs.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth=1;
    VkPipelineRasterizationConservativeStateCreateInfoEXT conservative{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT}; conservative.conservativeRasterizationMode=VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT;
    if (coverage) rs.pNext=&conservative;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
    std::array<VkPipelineColorBlendAttachmentState,2> masks{}; for (auto& m:masks) m.colorWriteMask=colors==2 ? VK_COLOR_COMPONENT_R_BIT : 15;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blend.attachmentCount=colors; blend.pAttachments=masks.data();
    const VkDynamicState dynamic[]{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dy.dynamicStateCount=2; dy.pDynamicStates=dynamic;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; gp.stageCount=2; gp.pStages=stages.data(); gp.pVertexInputState=&vi; gp.pInputAssemblyState=&ia; gp.pViewportState=&vp; gp.pRasterizationState=&rs; gp.pMultisampleState=&ms; gp.pColorBlendState=&blend; gp.pDynamicState=&dy; gp.layout=pipeline_layout; gp.renderPass=pass;
    VkPipeline result{}; const auto status=vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gp,nullptr,&result);
    for (auto m:modules) vkDestroyShaderModule(device,m,nullptr); checked(status,"Create native graphics pipeline"); return result;
}

void NativeRaster::Impl::initialize(unsigned family) {
    stats.ordered_fragment_access=ordered_fragments;
    VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(physical,&properties);
    unsigned families=0; vkGetPhysicalDeviceQueueFamilyProperties(physical,&families,nullptr);
    std::vector<VkQueueFamilyProperties> queue_properties(families); vkGetPhysicalDeviceQueueFamilyProperties(physical,&families,queue_properties.data());
    timestamp_bits=queue_properties.at(family).timestampValidBits; timestamp_period=properties.limits.timestampPeriod;
    stats.gpu_timing_supported=timestamp_bits!=0;
    if (timestamp_bits) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; qi.queryType=VK_QUERY_TYPE_TIMESTAMP; qi.queryCount=frame_slots*2;
        checked(vkCreateQueryPool(device,&qi,nullptr,&timestamps),"Create native GPU timestamps");
    }
    if (properties.limits.maxStorageBufferRange<record_bytes) throw std::runtime_error("Native draw records exceed storage buffer limit");
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pi.queueFamilyIndex=family;
    checked(vkCreateCommandPool(device,&pi,nullptr,&pool),"Create native command pool");
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool=pool; ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount=1;
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    for (auto& entry:slots) {
        checked(vkAllocateCommandBuffers(device,&ca,&entry.command),"Allocate native commands");
        checked(vkCreateFence(device,&fi,nullptr,&entry.fence),"Create native fence");
    }
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size=staging_bytes*frame_slots; bi.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
#ifdef ANYPS2_LLVM_TEXTURES
    bi.usage|=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
#endif
    checked(vkCreateBuffer(device,&bi,nullptr,&staging),"Create resource staging");
    VkMemoryRequirements requirements{}; vkGetBufferMemoryRequirements(device,staging,&requirements);
    staging_memory=allocate(requirements,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    checked(vkBindBufferMemory(device,staging,staging_memory,0),"Bind resource staging");
    checked(vkMapMemory(device,staging_memory,0,staging_bytes*frame_slots,0,&mapped),"Map resource staging");
    // Uploads favor sequential host writes; exports need CPU-readable cached
    // memory. Keep a separate bounded allocation rather than reading back from
    // a potentially write-combined upload heap. Every export waits before reuse.
    bi.size=2048ULL*2048*sizeof(std::uint32_t); bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    checked(vkCreateBuffer(device,&bi,nullptr,&readback),"Create native readback");
    vkGetBufferMemoryRequirements(device,readback,&requirements);
    readback_memory=allocate(requirements,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    checked(vkBindBufferMemory(device,readback,readback_memory,0),"Bind native readback");
    checked(vkMapMemory(device,readback_memory,0,bi.size,0,&readback_mapping),"Map native readback");
    bi.size=record_bytes; bi.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    checked(vkCreateBuffer(device,&bi,nullptr,&records),"Create native draw records");
    vkGetBufferMemoryRequirements(device,records,&requirements);
    record_memory=allocate(requirements,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    checked(vkBindBufferMemory(device,records,record_memory,0),"Bind native draw records");
    checked(vkMapMemory(device,record_memory,0,record_bytes,0,&record_mapping),"Map native draw records");
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; si.magFilter=si.minFilter=VK_FILTER_NEAREST;
    si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST; si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; si.maxLod=0;
    checked(vkCreateSampler(device,&si,nullptr,&sampler),"Create native sampler");
    std::array<VkDescriptorSetLayoutBinding,4> bindings{{{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {1,VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},{2,VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {3,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}}};
    if (ordered_fragments) bindings[1].descriptorType=bindings[2].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dl.bindingCount=4; dl.pBindings=bindings.data();
    checked(vkCreateDescriptorSetLayout(device,&dl,nullptr,&descriptors),"Create native descriptors");
    const VkDescriptorSetLayoutBinding pb{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}; dl.bindingCount=1; dl.pBindings=&pb;
    checked(vkCreateDescriptorSetLayout(device,&dl,nullptr,&present_descriptors),"Create display descriptors");
    std::array<VkDescriptorPoolSize,4> sizes{{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,32768},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,32768},{VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,32768},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,32768}}};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dp.maxSets=32768; dp.poolSizeCount=4; dp.pPoolSizes=sizes.data();
    checked(vkCreateDescriptorPool(device,&dp,nullptr,&descriptor_pool),"Create native descriptor pool");
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pl.setLayoutCount=1; pl.pSetLayouts=&descriptors;
    checked(vkCreatePipelineLayout(device,&pl,nullptr,&layout),"Create native layout");
    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(DisplayPlan)}; pl.pSetLayouts=&present_descriptors; pl.pushConstantRangeCount=1; pl.pPushConstantRanges=&push;
    checked(vkCreatePipelineLayout(device,&pl,nullptr,&present_layout),"Create native display layout");
    const std::array<VkDescriptorSetLayoutBinding,4> field_bindings{{
        {0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,3,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {2,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {3,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}}};
    dl.bindingCount=field_bindings.size(); dl.pBindings=field_bindings.data();
    checked(vkCreateDescriptorSetLayout(device,&dl,nullptr,&field_descriptors),"Create field descriptors");
    push.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT; pl.pSetLayouts=&field_descriptors;
    checked(vkCreatePipelineLayout(device,&pl,nullptr,&field_layout),"Create field reconstruction layout");
    VkShaderModuleCreateInfo field_module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    field_module_info.codeSize=sizeof(field_code); field_module_info.pCode=field_code;
    VkShaderModule field_module{};
    checked(vkCreateShaderModule(device,&field_module_info,nullptr,&field_module),"Create field shader");
    VkComputePipelineCreateInfo field_create{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    field_create.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,field_module,"main",nullptr};
    field_create.layout=field_layout;
    const auto field_result=vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&field_create,nullptr,&field_pipeline);
    vkDestroyShaderModule(device,field_module,nullptr);
    checked(field_result,"Create field reconstruction pipeline");
    std::array<VkAttachmentDescription,2> attachments{};
    for (auto& a:attachments) { a.format=VK_FORMAT_R32_UINT; a.samples=VK_SAMPLE_COUNT_1_BIT; a.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD; a.storeOp=VK_ATTACHMENT_STORE_OP_STORE; a.initialLayout=a.finalLayout=VK_IMAGE_LAYOUT_GENERAL; }
    const std::array<VkAttachmentReference,2> refs{{{0,VK_IMAGE_LAYOUT_GENERAL},{1,VK_IMAGE_LAYOUT_GENERAL}}};
    VkSubpassDescription sub{}; sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS; sub.colorAttachmentCount=2; sub.pColorAttachments=refs.data(); sub.inputAttachmentCount=2; sub.pInputAttachments=refs.data();
    VkSubpassDependency dep{}; dep.srcSubpass=dep.dstSubpass=0;
    dep.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; dep.dstStageMask=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; dep.dstAccessMask=VK_ACCESS_INPUT_ATTACHMENT_READ_BIT; dep.dependencyFlags=VK_DEPENDENCY_BY_REGION_BIT;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; rp.attachmentCount=2; rp.pAttachments=attachments.data(); rp.subpassCount=1; rp.pSubpasses=&sub; rp.dependencyCount=1; rp.pDependencies=&dep;
    if (ordered_fragments) {
        // Ordered shader accesses own colour/depth. No attachment load/store
        // operation may alias those images during the render pass.
        sub.colorAttachmentCount=sub.inputAttachmentCount=0;
        rp.attachmentCount=rp.dependencyCount=0;
    }
    checked(vkCreateRenderPass(device,&rp,nullptr,&render_pass),"Create native render pass");
    const auto code=ordered_fragments ? std::span<const std::uint32_t>(interlock_code) : std::span<const std::uint32_t>(fragment_code);
    pipeline=make_pipeline(render_pass,layout,vertex_code,code,ordered_fragments ? 0 : 2,false);
    coverage_pipeline=make_pipeline(render_pass,layout,vertex_code,code,ordered_fragments ? 0 : 2,true);
}
NativeRaster::Impl::~Impl() {
    vkDeviceWaitIdle(device);
#ifdef ANYPS2_LLVM_TEXTURES
    if (decode_pipeline) vkDestroyPipeline(device,decode_pipeline,nullptr);
    if (decode_layout) vkDestroyPipelineLayout(device,decode_layout,nullptr);
    if (decode_pool) vkDestroyDescriptorPool(device,decode_pool,nullptr);
    if (decode_descriptors) vkDestroyDescriptorSetLayout(device,decode_descriptors,nullptr);
    if (decoded) vkDestroyBuffer(device,decoded,nullptr);
    if (decoded_memory) vkFreeMemory(device,decoded_memory,nullptr);
#endif
    for (auto [k,f]:framebuffers) vkDestroyFramebuffer(device,f,nullptr);
    for (auto [k,f]:present_framebuffers) vkDestroyFramebuffer(device,f,nullptr);
    for (auto v:present_views) vkDestroyImageView(device,v,nullptr);
    textures.clear(); surfaces.clear();
    if (pipeline) vkDestroyPipeline(device,pipeline,nullptr);
    if (coverage_pipeline) vkDestroyPipeline(device,coverage_pipeline,nullptr);
    if (present_pipeline) vkDestroyPipeline(device,present_pipeline,nullptr);
    if (field_pipeline) vkDestroyPipeline(device,field_pipeline,nullptr);
    if (field_layout) vkDestroyPipelineLayout(device,field_layout,nullptr);
    if (field_descriptors) vkDestroyDescriptorSetLayout(device,field_descriptors,nullptr);
    if (render_pass) vkDestroyRenderPass(device,render_pass,nullptr);
    if (present_pass) vkDestroyRenderPass(device,present_pass,nullptr);
    if (layout) vkDestroyPipelineLayout(device,layout,nullptr);
    if (present_layout) vkDestroyPipelineLayout(device,present_layout,nullptr);
    if (descriptor_pool) vkDestroyDescriptorPool(device,descriptor_pool,nullptr);
    if (descriptors) vkDestroyDescriptorSetLayout(device,descriptors,nullptr);
    if (present_descriptors) vkDestroyDescriptorSetLayout(device,present_descriptors,nullptr);
    if (sampler) vkDestroySampler(device,sampler,nullptr);
    if (mapped) vkUnmapMemory(device,staging_memory);
    if (staging) vkDestroyBuffer(device,staging,nullptr);
    if (staging_memory) vkFreeMemory(device,staging_memory,nullptr);
    if (readback_mapping) vkUnmapMemory(device,readback_memory);
    if (readback) vkDestroyBuffer(device,readback,nullptr);
    if (readback_memory) vkFreeMemory(device,readback_memory,nullptr);
    if (record_mapping) vkUnmapMemory(device,record_memory);
    if (records) vkDestroyBuffer(device,records,nullptr);
    if (record_memory) vkFreeMemory(device,record_memory,nullptr);
    for (auto& entry:slots) if (entry.fence) vkDestroyFence(device,entry.fence,nullptr);
    if (pool) vkDestroyCommandPool(device,pool,nullptr);
    if (timestamps) vkDestroyQueryPool(device,timestamps,nullptr);
}
std::unique_ptr<NativeRaster::Impl::Image> NativeRaster::Impl::image(unsigned width,unsigned height) {
    auto i=std::make_unique<Image>(device,width,height);
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ci.imageType=VK_IMAGE_TYPE_2D; ci.format=VK_FORMAT_R32_UINT; ci.extent={width,height,1}; ci.mipLevels=ci.arrayLayers=1; ci.samples=VK_SAMPLE_COUNT_1_BIT; ci.tiling=VK_IMAGE_TILING_OPTIMAL;
    ci.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.usage|=VK_IMAGE_USAGE_STORAGE_BIT;
    checked(vkCreateImage(device,&ci,nullptr,&i->image),"Create native image"); VkMemoryRequirements mr{}; vkGetImageMemoryRequirements(device,i->image,&mr); i->allocation_bytes=mr.size; i->memory=allocate(mr,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    checked(vkBindImageMemory(device,i->image,i->memory,0),"Bind native image");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image=i->image; view.viewType=VK_IMAGE_VIEW_TYPE_2D; view.format=VK_FORMAT_R32_UINT; view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    checked(vkCreateImageView(device,&view,nullptr,&i->view),"Create native image view");
    order(); VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.image=i->image; b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout=VK_IMAGE_LAYOUT_GENERAL; b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT; b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; b.subresourceRange=view.subresourceRange;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b); return i;
}
void NativeRaster::Impl::upload_image(Image& image,std::span<const std::uint32_t> pixels) {
    Measure timing(stats.upload_ns,profiling); ++stats.uploads;
    if (pixels.size()!=std::size_t(image.width)*image.height || pixels.size_bytes()>staging_bytes) throw std::runtime_error("Invalid native image upload");
    // Append uploads to unused host-visible storage. Recorded transfers keep
    // their bytes until the submission fence completes; no per-texture stall.
    if (pixels.size_bytes()>staging_bytes-staging_used) flush();
    prepare_slot();
    const auto offset=slot*staging_bytes+staging_used;
    std::memcpy(static_cast<std::byte*>(mapped)+offset,pixels.data(),pixels.size_bytes());
    staging_used+=pixels.size_bytes(); order();
    VkBufferImageCopy copy{}; copy.bufferOffset=offset; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={image.width,image.height,1};
    vkCmdCopyBufferToImage(command,staging,image.image,VK_IMAGE_LAYOUT_GENERAL,1,&copy); stats.upload_bytes+=pixels.size_bytes(); image.valid=true;
}
void NativeRaster::Impl::export_image(Image& image) {
    if (!image.dirty || image.scratch) return;
    std::bitset<512> newer;
    for (unsigned page=0;page<512;++page)
        if (image.pages[page]>backing_revisions[page]) newer.set(page);
    if (newer.none()) { image.dirty=false; return; }
    Measure timing(stats.surface_export_ns,profiling);
    order();
    VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={image.width,image.height,1};
    vkCmdCopyImageToBuffer(command,image.image,VK_IMAGE_LAYOUT_GENERAL,readback,1,&copy); flush();
    // Host-visible staging may be uncached/write-combined. Read it once with a
    // contiguous copy before CPU address conversion instead of scalar loads
    // directly from the mapped allocation for every pixel.
    std::vector<std::uint32_t> pixels(std::size_t(image.width)*image.height);
    std::memcpy(pixels.data(),readback_mapping,pixels.size()*sizeof(pixels[0]));
    for (unsigned y=0;y<image.height;++y) for (unsigned x=0;x<image.width;++x) {
        const auto half=image.format ? rgba_halfword(image.base,image.pitch,x,y,image.format==10) : 0;
        const auto address=image.format ? half/2 : rgba_word(image.base,image.pitch,x,y)^(image.depth ? 0x600 : 0);
        if (newer[address/2048]) {
            const auto pixel=pixels[std::size_t(y)*image.width+x];
            if (image.format) {
                const auto shift=(half%2)*16;
                source[address]=(source[address]&~(65535U<<shift))|(pack_rgba16(pixel)<<shift);
            } else source[address]=pixel;
        }
    }
    for (unsigned page=0;page<512;++page) if (newer[page]) backing_revisions[page]=image.pages[page];
    // Once the writer becomes clean, surface() can no longer discover it via
    // dirty dependencies. Invalidate older aliases before retiring that link.
    for (auto& [key,other]:surfaces)
        if (other && other.get()!=&image && other->revision<image.revision && other->region.overlaps(image.region))
            other->valid=false;
    stats.readback_bytes+=std::size_t(image.width)*image.height*4; image.dirty=false;
}
void NativeRaster::Impl::synchronize_source(Region requested) {
    std::vector<Image*> ordered;
    for (auto& [key,i]:surfaces) if (i && i->dirty && i->region.overlaps(requested)) ordered.push_back(i.get());
    std::sort(ordered.begin(),ordered.end(),[](auto a,auto b){return a->revision<b->revision;});
    for (auto* i:ordered) export_image(*i);
}
void NativeRaster::Impl::clear_views() {
    flush(); for (auto [k,f]:framebuffers) vkDestroyFramebuffer(device,f,nullptr); framebuffers.clear(); sets.clear(); present_sets.clear();
    field_sets.clear();
    checked(vkResetDescriptorPool(device,descriptor_pool,0),"Reset native resource descriptors");
}
void NativeRaster::Impl::trim_surfaces() {
    // Retire descriptors and queued users before releasing an image. Keep the
    // current draw/display's acquired views alive while resolving its remaining
    // resources. Dirty pages must reach backing storage before their owner goes.
    clear_views();
    while (surfaces.size()>=48) {
        auto oldest=surfaces.end();
        for (auto it=surfaces.begin();it!=surfaces.end();++it)
            if (it->second && it->second->scope!=surface_scope &&
                (oldest==surfaces.end() || it->second->last_use<oldest->second->last_use)) oldest=it;
        if (oldest==surfaces.end()) throw std::runtime_error("Native surface cache has no unreferenced view");
        export_image(*oldest->second);
        // export_image can record transfers referencing this image.
        flush();
        surfaces.erase(oldest); ++stats.surface_evictions;
    }
}
void NativeRaster::Impl::copy_surface_pages(Image& destination,const Image& source,const std::bitset<512>& requested) {
    // A full storage page is 64x32 pixels. Depth's address XOR 0x600
    // permutes its four 32x16 tiles, without changing the page number.
    const bool permute=destination.depth!=source.depth;
    const unsigned tile_width=permute ? 32 : 64,tile_height=permute ? 16 : 32;
    std::vector<VkImageCopy> copies;
    std::bitset<512> copied;
    for (unsigned y=0;y<destination.height;y+=tile_height)
        for (unsigned x=0;x<destination.width;x+=tile_width) {
            const unsigned storage_page=(destination.base/32+(y/32)*destination.pitch+x/64)&511;
            if (!requested[storage_page]) continue;
            if (source.pages[storage_page]<=destination.pages[storage_page]) continue;
            const unsigned page=(storage_page-source.base/32)&511;
            const unsigned sx=(page%source.pitch)*64+((x%64)^(permute ? 32 : 0));
            const unsigned sy=(page/source.pitch)*32+((y%32)^(permute ? 16 : 0));
            if (sx+tile_width>source.width || sy+tile_height>source.height) continue;
            copied.set(storage_page);
            VkImageCopy copy{};
            copy.srcSubresource=copy.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
            copy.srcOffset={int(sx),int(sy),0}; copy.dstOffset={int(x),int(y),0};
            copy.extent={tile_width,tile_height,1};
            if (!copies.empty()) {
                auto& previous=copies.back();
                if (previous.srcOffset.y==copy.srcOffset.y && previous.dstOffset.y==copy.dstOffset.y &&
                    previous.srcOffset.x+int(previous.extent.width)==copy.srcOffset.x &&
                    previous.dstOffset.x+int(previous.extent.width)==copy.dstOffset.x) {
                    previous.extent.width+=tile_width; continue;
                }
            }
            copies.push_back(copy);
        }
    if (copies.empty()) return;
    // Contiguous pages are ordinary image rectangles. Merge adjacent rows
    // as well as columns instead of issuing a transfer region per GS tile.
    std::size_t count=0;
    for (const auto copy:copies) {
        if (count) {
            auto& previous=copies[count-1];
            if (previous.srcOffset.x==copy.srcOffset.x && previous.dstOffset.x==copy.dstOffset.x &&
                previous.extent.width==copy.extent.width &&
                previous.srcOffset.y+int(previous.extent.height)==copy.srcOffset.y &&
                previous.dstOffset.y+int(previous.extent.height)==copy.dstOffset.y) {
                previous.extent.height+=copy.extent.height; continue;
            }
        }
        copies[count++]=copy;
    }
    copies.resize(count);
    order();
    vkCmdCopyImage(command,source.image,VK_IMAGE_LAYOUT_GENERAL,destination.image,VK_IMAGE_LAYOUT_GENERAL,
        static_cast<unsigned>(copies.size()),copies.data());
    for (unsigned page=0;page<512;++page) if (copied[page]) destination.pages[page]=source.pages[page];
    destination.dirty=true;
    destination.revision=revision;
}
void NativeRaster::Impl::written(Image& image,const Sprite& draw) {
    image.dirty=true; image.revision=revision;
    // Allocated height is a cache capacity, not the footprint of this draw.
    // Full pages retain earlier pixels because surface() resolves aliases before
    // recording writes. Track only pages intersecting the clipped draw bounds.
    if (image.base%32) {
        const auto count=std::min(512U,((image.region.start%2048)+image.region.length+2047)/2048);
        for (unsigned i=0;i<count;++i) {
            const auto page=(image.region.start/2048+i)%512;
            image.pages[page]=source_revisions[page]=revision;
        }
    } else for (unsigned y=draw.top/(image.format ? 64:32);y<=(draw.bottom-1)/(image.format ? 64:32);++y)
        for (unsigned x=draw.left/64;x<=(draw.right-1)/64;++x) {
            const auto page=(image.base/32+y*image.pitch+x)&511;
            image.pages[page]=source_revisions[page]=revision;
        }
}
bool NativeRaster::Impl::current(const Image& image,const Sprite& draw) const {
    if (image.scratch) return true;
    if (!image.valid || image.base%32) return false;
    for (unsigned y=draw.top/(image.format ? 64:32);y<=(draw.bottom-1)/(image.format ? 64:32);++y)
        for (unsigned x=draw.left/64;x<=(draw.right-1)/64;++x) {
            const auto page=(image.base/32+y*image.pitch+x)&511;
            if (image.pages[page]!=source_revisions[page]) return false;
        }
    return true;
}
NativeRaster::Impl::Image& NativeRaster::Impl::surface(unsigned base,unsigned pitch,unsigned height,bool depth,bool scratch,bool display,const Sprite* draw,unsigned format) {
    Measure timing(stats.surface_lookup_ns,profiling);
    const auto key=std::tuple(scratch ? 0x10000U : base|(display ? 0x20000U : 0U),pitch,depth ? 1U : format); const unsigned width=pitch*64;
    const auto page_height=format ? 64U : 32U;
    height=(height+page_height-1)&~(page_height-1);
    if (!pitch || width>4032 || !height || height>2048) throw std::runtime_error("Invalid native render surface dimensions");
    if (!surfaces.contains(key) && surfaces.size()>=64) trim_surfaces();
    if (std::uint64_t(width)*height>store_words)
        throw std::runtime_error("Aliased oversized framebuffer requires explicit resource conversion");
    auto& slot=surfaces[key];
    if (slot && slot->height<height) { export_image(*slot); clear_views(); slot.reset(); }
    if (slot) { slot->last_use=++surface_use; slot->scope=surface_scope; }
    if (slot && draw && current(*slot,*draw)) return *slot;
    const auto requested=region(base,pitch,width,(slot ? slot->height : height)/(format ? 2:1));
    std::bitset<512> requested_pages;
    if (draw && base%32==0) {
        for (unsigned y=draw->top/page_height;y<=(draw->bottom-1)/page_height;++y)
            for (unsigned x=draw->left/64;x<=(draw->right-1)/64;++x)
                requested_pages.set((base/32+y*pitch+x)&511);
    } else for (unsigned page=0;page<512;++page)
        if (requested.overlaps({page*2048,2048})) requested_pages.set(page);
    bool compose=!scratch && !format && base%32==0 && width<=2048;
    std::vector<Image*> writers;
    if (compose) {
        for (auto& [other_key,other]:surfaces) if (other && other->dirty && other->region.overlaps(requested)) {
            if (!other->valid || other->scratch || other->format || other->base%32 || other->width>2048) { compose=false; break; }
            writers.push_back(other.get());
        }
        std::sort(writers.begin(),writers.end(),[](auto a,auto b){return a->revision<b->revision;});
    }
    if (!slot) {
        if (!scratch && !compose) synchronize_source(requested); slot=image(width,height);
        slot->base=base; slot->pitch=pitch; slot->depth=depth; slot->scratch=scratch; slot->format=format;
        slot->region=scratch ? Region{} : requested; slot->valid=false;
        slot->last_use=++surface_use; slot->scope=surface_scope;
    }
    for (auto& [other_key,other]:surfaces) if (other && other.get()!=slot.get() && other->dirty && other->region.overlaps(slot->region) &&
        [&] { for (unsigned page=0;page<512;++page)
            if (requested_pages[page] && other->pages[page]>slot->pages[page]) return true;
            return false; }()) {
        ++stats.surface_conflicts;
        stats.last_surface_conflict={slot->base,slot->pitch,slot->height,unsigned(slot->depth),
            other->base,other->pitch,other->height,unsigned(other->depth)};
        if (!compose) { synchronize_source(slot->region); slot->valid=false; }
        break;
    }
    bool changed=!slot->valid;
    if (!slot->valid) {
        Measure timing(stats.surface_import_ns,profiling);
        std::vector<std::uint32_t> pixels(std::size_t(width)*slot->height);
        if (!scratch) for (unsigned y=0;y<slot->height;++y) for (unsigned x=0;x<width;++x) {
            const auto half=format ? rgba_halfword(base,pitch,x,y,format==10) : 0;
            pixels[std::size_t(y)*width+x]=format ? expand_rgba16((source[half/2]>>((half%2)*16))&65535U) :
                source[rgba_word(base,pitch,x,y)^(depth ? 0x600 : 0)];
        }
        upload_image(*slot,pixels);
        slot->pages=backing_revisions;
        slot->dirty=false;
    }
    // Preserve the target's existing pixels and merge only newer overlapping
    // writers, in publication order. Page-aligned color/depth aliases can share
    // native GPU copies; CPU consumers retain the explicit export path.
    if (compose) for (const auto* writer:writers) if (writer!=slot.get()) {
        copy_surface_pages(*slot,*writer,requested_pages);
    }
    if (changed) slot->revision=revision;
    return *slot;
}
NativeRaster::Impl::Image& NativeRaster::Impl::display_surface(unsigned base,unsigned pitch,unsigned height) {
    const auto found=surfaces.find({base,pitch,false});
    if (found!=surfaces.end() && found->second && found->second->height>=height)
        return surface(base,pitch,height,false);
    // Display offsets can read beyond the drawing extent, even into adjacent
    // storage. Such a read must not permanently enlarge the writable target:
    // that would make later unrelated draws conflict through the extra rows.
    // This separately cached view is never dirtied by drawing. Page-aligned
    // aliases are composed on the GPU; other layouts synchronize backing data.
    return surface(base,pitch,height,false,false,true);
}

#ifdef ANYPS2_LLVM_TEXTURES
void NativeRaster::Impl::initialize_texture_decoder() {
    VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(physical,&properties);
    decode_alignment=std::max(VkDeviceSize(4),properties.limits.minStorageBufferOffsetAlignment);
    if (properties.limits.maxStorageBufferRange<(store_words+262)*sizeof(std::uint32_t))
        throw std::runtime_error("LLVM texture kernel exceeds storage buffer range");
    std::array<VkDescriptorSetLayoutBinding,2> bindings{};
    for (unsigned n=0;n<2;++n) bindings[n]={n,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    VkDescriptorSetLayoutCreateInfo descriptors_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    descriptors_info.bindingCount=2; descriptors_info.pBindings=bindings.data();
    checked(vkCreateDescriptorSetLayout(device,&descriptors_info,nullptr,&decode_descriptors),"Create LLVM texture descriptors");
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,static_cast<unsigned>(decode_sets.size()*2)};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets=static_cast<unsigned>(decode_sets.size()); pool_info.poolSizeCount=1; pool_info.pPoolSizes=&size;
    checked(vkCreateDescriptorPool(device,&pool_info,nullptr,&decode_pool),"Create LLVM texture descriptor pool");
    std::array<VkDescriptorSetLayout,frame_slots*decode_sets_per_slot> layouts{}; layouts.fill(decode_descriptors);
    VkDescriptorSetAllocateInfo allocate_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate_info.descriptorPool=decode_pool; allocate_info.descriptorSetCount=static_cast<unsigned>(layouts.size()); allocate_info.pSetLayouts=layouts.data();
    checked(vkAllocateDescriptorSets(device,&allocate_info,decode_sets.data()),"Allocate LLVM texture descriptor sets");
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount=1; layout_info.pSetLayouts=&decode_descriptors;
    checked(vkCreatePipelineLayout(device,&layout_info,nullptr,&decode_layout),"Create LLVM texture pipeline layout");
    VkShaderModuleCreateInfo shader_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shader_info.codeSize=sizeof(texture_decode_code); shader_info.pCode=texture_decode_code;
    VkShaderModule shader{}; checked(vkCreateShaderModule(device,&shader_info,nullptr,&shader),"Load AOT LLVM texture kernel");
    VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline_info.layout=decode_layout; pipeline_info.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    pipeline_info.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; pipeline_info.stage.module=shader; pipeline_info.stage.pName="main";
    const auto status=vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pipeline_info,nullptr,&decode_pipeline);
    vkDestroyShaderModule(device,shader,nullptr); checked(status,"Create AOT LLVM texture pipeline");
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size=store_words*sizeof(std::uint32_t); buffer_info.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    checked(vkCreateBuffer(device,&buffer_info,nullptr,&decoded),"Create GPU texture conversion output");
    VkMemoryRequirements requirements{}; vkGetBufferMemoryRequirements(device,decoded,&requirements);
    decoded_memory=allocate(requirements,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    checked(vkBindBufferMemory(device,decoded,decoded_memory,0),"Bind GPU texture conversion output");
    stats.llvm_texture_kernels=true;
}
void NativeRaster::Impl::decode_texture(Image& image,const SpriteCommand& draw,Region source_region) {
    Measure timing(stats.texture_decode_ns,profiling);
    const auto count=std::uint64_t(image.width)*image.height;
    if (!count || count>store_words || !source_region.length || source_region.length>store_words)
        throw std::runtime_error("Invalid LLVM texture conversion extent");
    const auto bytes=VkDeviceSize(source_region.length+262)*sizeof(std::uint32_t);
    order();
    const auto verification_bytes=verify_decodes ? count*sizeof(std::uint32_t) : 0;
    if (decodes_used==decode_sets_per_slot ||
        bytes+verification_bytes+decode_alignment-1>staging_bytes-staging_used) {
        flush(VK_NULL_HANDLE,VK_NULL_HANDLE,false); begin();
    }
    const auto local=(staging_used+decode_alignment-1)/decode_alignment*decode_alignment;
    const auto offset=slot*staging_bytes+local;
    auto* input=reinterpret_cast<std::uint32_t*>(static_cast<std::byte*>(mapped)+offset);
    const std::array<std::uint32_t,6> header{static_cast<unsigned>(count),image.width,draw.draw.texture_base,
        draw.draw.texture_pitch,(draw.draw.flags&indexed8_flag) ? 2U : (draw.draw.flags&64) ? 1U : 0U,source_region.start};
    std::copy(header.begin(),header.end(),input);
    std::copy(draw.palette.begin(),draw.palette.end(),input+6);
    const auto first=std::min(source_region.length,store_words-source_region.start);
    std::copy_n(source.data()+source_region.start,first,input+262);
    std::copy_n(source.data(),source_region.length-first,input+262+first);
    staging_used=local+bytes;
    const auto set=decode_sets[slot*decode_sets_per_slot+decodes_used++];
    const std::array<VkDescriptorBufferInfo,2> infos{{{staging,offset,bytes},{decoded,0,count*sizeof(std::uint32_t)}}};
    std::array<VkWriteDescriptorSet,2> writes{};
    for (unsigned n=0;n<2;++n) {
        writes[n]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; writes[n].dstSet=set; writes[n].dstBinding=n;
        writes[n].descriptorCount=1; writes[n].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[n].pBufferInfo=&infos[n];
    }
    vkUpdateDescriptorSets(device,2,writes.data(),0,nullptr);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,decode_pipeline);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,decode_layout,0,1,&set,0,nullptr);
    vkCmdDispatch(command,(static_cast<unsigned>(count)+63)/64,1,1);
    order();
    VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={image.width,image.height,1};
    vkCmdCopyBufferToImage(command,decoded,image.image,VK_IMAGE_LAYOUT_GENERAL,1,&copy);
    stats.upload_bytes+=bytes; ++stats.texture_kernel_dispatches; image.valid=true;
    if (verify_decodes) {
        const auto readback_offset=slot*staging_bytes+staging_used;
        staging_used+=verification_bytes;
        const VkBufferCopy readback{0,readback_offset,verification_bytes};
        vkCmdCopyBuffer(command,decoded,staging,1,&readback);
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
        flush();
        const auto* pixels=reinterpret_cast<const std::uint32_t*>(static_cast<const std::byte*>(mapped)+readback_offset);
        for (unsigned y=0;y<image.height;++y) for (unsigned x=0;x<image.width;++x) {
            const auto& d=draw.draw;
            const auto address=(d.flags&indexed8_flag) ? indexed_byte(d.texture_base,d.texture_pitch,x,y)*2 : (d.flags&64) ? indexed_nibble(d.texture_base,d.texture_pitch,x,y) : rgba_word(d.texture_base,d.texture_pitch,x,y)*8;
            const auto raw=source[address/8];
            const auto expected=(d.flags&64) ? draw.palette[(raw>>((address%8)*4))&((d.flags&indexed8_flag) ? 255 : 15)] : raw;
            if (pixels[std::size_t(y)*image.width+x]!=expected)
                throw std::runtime_error("LLVM texture kernel mismatch at texel "+std::to_string(x)+","+std::to_string(y));
        }
        stats.readback_bytes+=verification_bytes; ++stats.texture_kernel_verified;
    }
}
#endif
NativeRaster::Impl::Image& NativeRaster::Impl::texture(const SpriteCommand& command) {
    Measure timing(stats.texture_lookup_ns,profiling);
    const auto& d=command.draw;
    unsigned width=(d.flags&1) ? texture_extent(d.texture_width,d.clamp_u,d.min_u,d.max_u) : 1;
    unsigned height=(d.flags&1) ? texture_extent(d.texture_height,d.clamp_v,d.min_v,d.max_v) : 1;
    if ((d.flags&1) && !(d.flags&64)) {
        const auto found=surfaces.find({d.texture_base,d.texture_pitch,false});
        if (found!=surfaces.end() && found->second && found->second->valid &&
            texture_fits_surface(d,found->second->width,found->second->height)) {
            width=std::min(width,found->second->width);
            height=std::min(height,found->second->height);
        }
    }
    std::array<std::uint32_t,261> key{d.texture_base,d.texture_pitch,width,height,d.flags&(65|indexed8_flag)};
    if (d.flags&64) std::copy(command.palette.begin(),command.palette.end(),key.begin()+5);
    const auto r=(d.flags&1) ? region(d.texture_base,d.texture_pitch,width,height,(d.flags&indexed8_flag) ? 2U : (d.flags&64) ? 1U : 0U) : Region{};
    auto found_texture=textures.find(key);
    if (found_texture==textures.end()) {
        auto allocated=image(width,height);
        constexpr VkDeviceSize budget=128ULL*1024*1024;
        if (texture_bytes+allocated->allocation_bytes>budget || textures.size()>=1024) {
            // Retire GPU users and descriptors before releasing old images.
            // Keep recent textures instead of discarding the entire working set.
            clear_views();
            while (!textures.empty() && (texture_bytes+allocated->allocation_bytes>budget*3/4 || textures.size()>=768)) {
                const auto oldest=std::min_element(textures.begin(),textures.end(),[](const auto& a,const auto& b) {
                    return a.second.last_use<b.second.last_use;
                });
                texture_bytes-=oldest->second.image->allocation_bytes;
                textures.erase(oldest); ++stats.texture_evictions;
            }
        }
        texture_bytes+=allocated->allocation_bytes;
        found_texture=textures.emplace(key,Texture{std::move(allocated),r}).first;
        ++stats.texture_misses;
    }
    auto& tex=found_texture->second; tex.last_use=++texture_use;
    const auto generation=source_revision(r);
    if (generation!=tex.source_revision) { tex.valid=false; tex.source_revision=generation; }
    // Identical surface views use an ordered native image copy for feedback.
    // No CPU conversion is involved in the common framebuffer texture path.
    if ((d.flags&1) && !(d.flags&64)) {
        auto found=surfaces.find({d.texture_base,d.texture_pitch,false});
        if (found!=surfaces.end() && found->second && found->second->valid &&
            found->second->width>=width && found->second->height>=height) {
            // A draw may have resolved only its touched pages. Sampling the
            // image must first resolve all pages of the requested source view.
            auto& from=surface(d.texture_base,d.texture_pitch,height,false);
            if (!tex.valid || tex.image->revision!=from.revision) {
                order(); VkImageCopy copy{}; copy.srcSubresource=copy.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.extent={width,height,1};
                vkCmdCopyImage(this->command,from.image,VK_IMAGE_LAYOUT_GENERAL,tex.image->image,VK_IMAGE_LAYOUT_GENERAL,1,&copy);
                tex.image->revision=from.revision; tex.valid=true; stats.texture_snapshot_bytes+=std::size_t(width)*height*4;
            }
            return *tex.image;
        }
    }
    for (auto& [k,i]:surfaces) if (i && i->dirty && i->region.overlaps(r)) { ++stats.texture_readbacks; synchronize_source(r); tex.valid=false; break; }
    if (!tex.valid) {
#ifdef ANYPS2_LLVM_TEXTURES
        if (d.flags&1) {
            decode_texture(*tex.image,command,r); tex.valid=true;
            return *tex.image;
        }
#endif
        std::vector<std::uint32_t> pixels(std::size_t(width)*height);
        { Measure timing(stats.texture_decode_ns,profiling);
        if (d.flags&1) for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x) {
            std::uint32_t value;
            if (d.flags&indexed8_flag) { const auto byte=indexed_byte(d.texture_base,d.texture_pitch,x,y); value=command.palette[(source[byte/4]>>((byte%4)*8))&255]; }
            else if (d.flags&64) { const auto nibble=indexed_nibble(d.texture_base,d.texture_pitch,x,y); value=command.palette[(source[nibble/8]>>((nibble%8)*4))&15]; }
            else value=source[rgba_word(d.texture_base,d.texture_pitch,x,y)];
            pixels[std::size_t(y)*width+x]=value;
        }
        }
        upload_image(*tex.image,pixels); tex.valid=true;
    }
    return *tex.image;
}
VkDescriptorSet NativeRaster::Impl::allocate_set(VkDescriptorSetLayout set_layout) {
    VkDescriptorSetAllocateInfo a{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; a.descriptorPool=descriptor_pool; a.descriptorSetCount=1; a.pSetLayouts=&set_layout;
    VkDescriptorSet set{}; checked(vkAllocateDescriptorSets(device,&a,&set),"Allocate native resource set"); return set;
}
VkDescriptorSet NativeRaster::Impl::bind_target(Image& color,Image& depth,Image& texture,const Sprite& draw) {
    Measure timing(stats.target_bind_ns,profiling);
    if (sets.size()>=8192) clear_views();
    auto& fb=framebuffers[{&color,&depth}];
    const unsigned height=std::min(color.height,depth.height);
    if (!fb) {
        const VkImageView views[]{color.view,depth.view}; VkFramebufferCreateInfo f{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; f.renderPass=render_pass; f.attachmentCount=2; f.pAttachments=views; f.width=color.width; f.height=height; f.layers=1;
        if (ordered_fragments) f.attachmentCount=0;
        checked(vkCreateFramebuffer(device,&f,nullptr,&fb),"Create native framebuffer");
    }
    if (!in_pass || current_framebuffer!=fb) {
        order(); VkRenderPassBeginInfo b{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; b.renderPass=render_pass; b.framebuffer=fb; b.renderArea.extent={color.width,height};
        vkCmdBeginRenderPass(command,&b,VK_SUBPASS_CONTENTS_INLINE); in_pass=true; current_framebuffer=fb; current_width=color.width; current_height=height;
        const VkViewport viewport{0,0,float(current_width),float(current_height),0,1};
        const VkRect2D scissor{{0,0},{current_width,current_height}};
        vkCmdSetViewport(command,0,1,&viewport); vkCmdSetScissor(command,0,1,&scissor);
    } else if (!ordered_fragments && hazards.overlaps(draw.left,draw.top,draw.right,draw.bottom)) {
        flush_batch();
        hazards.clear();
        ++stats.feedback_barriers;
        VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; b.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; b.dstAccessMask=VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_DEPENDENCY_BY_REGION_BIT,1,&b,0,nullptr,0,nullptr);
    }
    if (!ordered_fragments) hazards.add(draw.left,draw.top,draw.right,draw.bottom);
    auto& set=sets[{&color,&depth,&texture}];
    if (!set) {
        set=allocate_set(descriptors);
        const VkDescriptorImageInfo images[]{{sampler,texture.view,VK_IMAGE_LAYOUT_GENERAL},{VK_NULL_HANDLE,color.view,VK_IMAGE_LAYOUT_GENERAL},{VK_NULL_HANDLE,depth.view,VK_IMAGE_LAYOUT_GENERAL}};
        std::array<VkWriteDescriptorSet,4> writes{};
        for (unsigned i=0;i<3;++i) { writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet=set; writes[i].dstBinding=i; writes[i].descriptorCount=1; writes[i].descriptorType=i ? VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[i].pImageInfo=&images[i]; }
        if (ordered_fragments) writes[1].descriptorType=writes[2].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        const VkDescriptorBufferInfo buffer{records,0,record_bytes};
        writes[3].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[3].dstSet=set; writes[3].dstBinding=3; writes[3].descriptorCount=1;
        writes[3].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[3].pBufferInfo=&buffer;
        vkUpdateDescriptorSets(device,4,writes.data(),0,nullptr);
    }
    return set;
}
NativeRaster::NativeRaster(VkPhysicalDevice p,VkDevice d,VkQueue q,unsigned family,bool ordered):impl_(std::make_unique<Impl>(p,d,q,ordered)) {
    impl_->initialize(family);
#ifdef ANYPS2_LLVM_TEXTURES
    impl_->initialize_texture_decoder();
#endif
}
NativeRaster::~NativeRaster()=default;
const RenderStatistics& NativeRaster::statistics() const { return impl_->stats; }
void NativeRaster::profile_commands(bool enable) { impl_->profiling=enable; }
void NativeRaster::verify_texture_kernels(bool enable) {
#ifdef ANYPS2_LLVM_TEXTURES
    impl_->verify_decodes=enable;
#else
    if (enable) throw std::runtime_error("LLVM texture verification requires ANYPS2_LLVM_TEXTURES");
#endif
}
void NativeRaster::finish() { impl_->flush(); }
std::uint64_t NativeRaster::enqueue_finish() { impl_->flush(VK_NULL_HANDLE,VK_NULL_HANDLE,false); return impl_->submitted; }
std::uint64_t NativeRaster::completed_submission() {
    for (unsigned i=0;i<frame_slots;++i) impl_->collect(i,false);
    return impl_->completed;
}
void NativeRaster::upload(std::span<const std::uint32_t> words) {
    if (words.size()!=store_words) throw std::runtime_error("Invalid native resource import");
    auto& r=*impl_; r.clear_views(); r.surfaces.clear(); r.textures.clear(); r.texture_bytes=0; r.source_revisions={}; r.backing_revisions={}; std::copy(words.begin(),words.end(),r.source.begin()); r.initialized=true; ++r.revision;
    r.field_count=0; r.previous_field=DisplayField::progressive; r.resolved_revision=0;
}
void NativeRaster::read(std::span<std::uint32_t> words) {
    if (words.size()!=store_words) throw std::runtime_error("Invalid native resource export");
    auto& r=*impl_; r.synchronize_source({0,store_words}); r.flush(); std::copy(r.source.begin(),r.source.end(),words.begin());
}
void NativeRaster::read_words(std::span<const unsigned> addresses,std::span<std::uint32_t> values) {
    if (addresses.size()!=values.size()) throw std::runtime_error("Invalid native palette read");
    for (auto address:addresses) if (address>=store_words) throw std::runtime_error("Invalid native palette address");
    auto& r=*impl_; for (unsigned i=0;i<addresses.size();++i) {
        for (const auto& [key,surface]:r.surfaces) if (surface && surface->dirty && surface->region.contains(addresses[i])) {
            ++r.stats.palette_readbacks; break;
        }
        r.synchronize_source({addresses[i],1}); values[i]=r.source[addresses[i]];
    }
}
void NativeRaster::write(std::span<const PixelWrite> writes) {
    for (auto w:writes) if (w.address>=store_words || (w.value&~w.mask)) throw std::runtime_error("Invalid native resource write");
    auto& r=*impl_; Measure timing(r.stats.write_ns,r.profiling);
    for (auto& [k,s]:r.surfaces) if (s && s->dirty && std::any_of(writes.begin(),writes.end(),[&](auto w){return s->region.contains(w.address);})) r.synchronize_source(s->region);
    unsigned first=store_words,last=0;
    for (auto w:writes) {
        auto& value=r.source[w.address]; const auto next=(value&~w.mask)|w.value; if (value==next) continue;
        value=next; first=std::min(first,w.address); last=std::max(last,w.address);
    }
    if (first!=store_words) {
        // Conservative changed range; invalidate each view once per upload,
        // not once per pixel. Sparse/overlapping/masked writes stay ordered.
        const Region changed{first,last-first+1};
        for (auto& [k,s]:r.surfaces) if (s && s->region.overlaps(changed)) s->valid=false;
        ++r.revision; r.touch(changed);
        for (unsigned page=first/2048;page<=last/2048;++page) r.backing_revisions[page]=r.revision;
    }
}
void NativeRaster::draw(std::span<const SpriteCommand> commands) {
    VulkanRenderer::validate(commands); auto& r=*impl_; Measure timing(r.stats.draw_ns,r.profiling);
    if (!r.initialized) throw std::runtime_error("Native resources not initialized");
    for (const auto& c:commands) {
        const auto& d=c.draw;
        if (d.left!=d.right && d.top!=d.bottom && d.right>d.pitch*64 && !can_map_wrapped_sprite(d))
            throw std::runtime_error("Wrapped textured, depth or self-aliasing framebuffer requires an explicit native resource mapping");
    }
    // A draw-only run cannot modify a disjoint texture. Resolve resources once
    // for compatible consecutive draws, but retain the ordinary alias/feedback
    // path whenever a target can write texture storage or another attachment.
    Impl::Image *cached_color=nullptr,*cached_depth=nullptr,*cached_texture=nullptr;
    SpriteCommand cached_command{};
    const auto same_texture=[](const SpriteCommand& a,const SpriteCommand& b) {
        const auto& x=a.draw; const auto& y=b.draw;
        constexpr unsigned formats=65|indexed8_flag;
        return (x.flags&formats)==(y.flags&formats) &&
            x.texture_base==y.texture_base && x.texture_pitch==y.texture_pitch &&
            x.texture_width==y.texture_width && x.texture_height==y.texture_height &&
            x.clamp_u==y.clamp_u && x.clamp_v==y.clamp_v &&
            x.min_u==y.min_u && x.max_u==y.max_u && x.min_v==y.min_v && x.max_v==y.max_v &&
            (!(x.flags&64) || a.palette==b.palette);
    };
    const auto emit=[&](SpriteCommand c,unsigned height) {
        ++r.surface_scope;
        // An enabled NEVER alpha test always takes its failure action. FB_ONLY
        // and RGB_ONLY cannot write depth. With an ALWAYS depth comparison they
        // do not read it either: avoid creating dependencies on an unused image.
        const auto failure=(c.draw.test>>12)&3U;
        if ((c.draw.test&15U)==1U && (failure==1 || failure==3))
            c.draw.flags&=~depth_write_flag;
        if ((c.draw.test&15U)==1U && failure==2) c.draw.mask=0xffffffffU;
        if (((c.draw.test>>17)&3U)==1U && !(c.draw.flags&depth_write_flag))
            c.draw.flags&=~depth_test_flag;
        const auto& d=c.draw;
        if (r.records_used==records_per_slot) r.flush(VK_NULL_HANDLE,VK_NULL_HANDLE,false);
        const auto& previous=cached_command.draw;
        const bool reuse=cached_color && previous.base==d.base && previous.pitch==d.pitch &&
            (previous.flags&(color_16_flag|color_16s_flag))==(d.flags&(color_16_flag|color_16s_flag)) &&
            (previous.flags&depth_test_flag)==(d.flags&depth_test_flag) &&
            (!(d.flags&depth_test_flag) || previous.depth_base==d.depth_base) &&
            cached_color->height>=height && cached_depth->height>=height && same_texture(c,cached_command) &&
            r.current(*cached_color,d) && r.current(*cached_depth,d);
        const unsigned format=(d.flags&color_16_flag) ? ((d.flags&color_16s_flag) ? 10 : 2) : 0;
        auto& color=reuse ? *cached_color : r.surface(d.base,d.pitch,height,false,false,false,&d,format);
        color.scope=r.surface_scope; color.last_use=++r.surface_use;
        auto& depth=reuse ? *cached_depth : r.surface((d.flags&depth_test_flag) ? d.depth_base : 0,d.pitch,height,true,!(d.flags&depth_test_flag),false,&d);
        depth.scope=r.surface_scope; depth.last_use=++r.surface_use;
        auto& texture=reuse ? *cached_texture : r.texture(c);
        if (!reuse) {
            cached_color=nullptr;
            const auto source=(d.flags&1) ? region(d.texture_base,d.texture_pitch,texture.width,texture.height,
                (d.flags&indexed8_flag) ? 2U : (d.flags&64) ? 1U : 0U) : Region{};
            const bool full_texture=!(d.flags&1) ||
                (texture.width==texture_extent(d.texture_width,d.clamp_u,d.min_u,d.max_u) &&
                 texture.height==texture_extent(d.texture_height,d.clamp_v,d.min_v,d.max_v));
            if (full_texture && color.valid && depth.valid && !color.region.overlaps(depth.region) &&
                !source.overlaps(color.region) && !source.overlaps(depth.region)) {
                cached_color=&color; cached_depth=&depth; cached_texture=&texture; cached_command=c;
            }
        }
        const auto set=r.bind_target(color,depth,texture,d);
        { Measure timing(r.stats.draw_record_ns,r.profiling); r.append_draw(set,c); }
        ++r.revision;
        { Measure timing(r.stats.invalidate_ns,r.profiling);
        if (d.mask!=0xffffffffU) r.written(color,d);
        if ((d.flags&depth_test_flag) && (d.flags&depth_write_flag)) r.written(depth,d);
        }
        if (d.flags&triangle_flag) ++r.stats.triangles; else ++r.stats.sprites;
    };
    for (const auto& c:commands) {
        const auto& d=c.draw;
        if (d.left==d.right || d.top==d.bottom) continue;
        if (d.right<=d.pitch*64) { emit(c,d.bottom); continue; }
        const auto mapped=map_wrapped_sprite(c);
        const auto height=mapped.commands[mapped.count-1].draw.bottom;
        for (unsigned i=0;i<mapped.count;++i) emit(mapped.commands[i],height);
    }
}
NativeRaster::Impl::Image& NativeRaster::Impl::reconstruct(const DisplayPlan& plan) {
    ++surface_scope;
    if (!initialized || !plan.width || !plan.height || plan.width>2048 || plan.height>2048)
        throw std::runtime_error("Invalid native field output");
    if (display_image && resolved_revision==revision && std::memcmp(&resolved_plan,&plan,sizeof(plan))==0)
        return *display_image;
    if (!display_image || display_image->width!=plan.width || display_image->height!=plan.height) {
        clear_views();
        display_image=image(plan.width,plan.height);
        for (auto& field:fields) field=image(plan.width,plan.height);
        field_count=0; field_index=0; previous_field=DisplayField::progressive; resolved_revision=0;
    }
    Image* inputs[2]{};
    for (unsigned i=0;i<2;++i) if (plan.control&(1U<<i)) {
        const auto& s=plan.surfaces[i];
        inputs[i]=&display_surface(s.base,s.pitch,s.y+(s.height+1)/2);
    }
    if (!inputs[0]) inputs[0]=inputs[1];
    if (!inputs[1]) inputs[1]=inputs[0];
    if (!inputs[0]) throw std::runtime_error("Field output has no source");
    const std::array<std::uint64_t,2> source_revisions{inputs[0]->revision,inputs[1]->revision};
    auto content_plan=plan; content_plan.field=resolved_plan.field;
    if (resolved_revision && source_revisions==resolved_sources &&
        std::memcmp(&resolved_plan,&content_plan,sizeof(plan))==0) {
        // A display deadline is not a new field of content. Reconstructing an
        // unchanged source at the opposite parity moves a stationary image.
        // Retain its progressive result, but break temporal history across the
        // missing field so subsequent motion cannot weave with stale samples.
        if (plan.field!=resolved_plan.field) { field_count=0; previous_field=DisplayField::progressive; }
        return *display_image;
    }
    if (previous_field==plan.field || previous_field==DisplayField::progressive) field_count=0;
    auto& set=field_sets[{inputs[0],inputs[1],field_index}];
    if (!set) {
        set=allocate_set(field_descriptors);
        std::array<VkDescriptorImageInfo,7> info{};
        for (unsigned i=0;i<2;++i) info[i]={sampler,inputs[i]->view,VK_IMAGE_LAYOUT_GENERAL};
        for (unsigned i=0;i<3;++i) info[2+i]={sampler,fields[(field_index+3-i)%4]->view,VK_IMAGE_LAYOUT_GENERAL};
        info[5]={VK_NULL_HANDLE,fields[field_index]->view,VK_IMAGE_LAYOUT_GENERAL};
        info[6]={VK_NULL_HANDLE,display_image->view,VK_IMAGE_LAYOUT_GENERAL};
        std::array<VkWriteDescriptorSet,4> writes{};
        const unsigned counts[]{2,3,1,1},offsets[]{0,2,5,6};
        for (unsigned i=0;i<4;++i) {
            auto& w=writes[i]; w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet=set; w.dstBinding=i; w.descriptorCount=counts[i]; w.pImageInfo=info.data()+offsets[i];
            w.descriptorType=i<2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        }
        vkUpdateDescriptorSets(device,writes.size(),writes.data(),0,nullptr);
    }
    auto constants=plan; constants.history_valid=(field_count>=3 ? 1U : 0U)|(resolved_revision ? 2U : 0U);
    order();
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,field_pipeline);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,field_layout,0,1,&set,0,nullptr);
    vkCmdPushConstants(command,field_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(constants),&constants);
    vkCmdDispatch(command,(plan.width+7)/8,(plan.height+7)/8,1);
    order();
    field_index=(field_index+1)%4; field_count=std::min(field_count+1,3U); previous_field=plan.field;
    resolved_plan=plan; resolved_revision=revision; resolved_sources=source_revisions;
    return *display_image;
}
Scanout NativeRaster::capture(const DisplayPlan& plan) {
    auto& r=*impl_;
    if (plan.field!=DisplayField::progressive) {
        auto& output=r.reconstruct(plan); r.order();
        VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={plan.width,plan.height,1};
        vkCmdCopyImageToBuffer(r.command,output.image,VK_IMAGE_LAYOUT_GENERAL,r.readback,1,&copy); r.flush();
        Scanout frame{plan.width,plan.height,std::vector<std::uint32_t>(std::size_t(plan.width)*plan.height)};
        std::memcpy(frame.pixels.data(),r.readback_mapping,frame.pixels.size()*4);
        r.stats.readback_bytes+=frame.pixels.size()*4;
        return frame;
    }
    r.field_count=0; r.previous_field=DisplayField::progressive; r.resolved_revision=0;
    r.synchronize_source({0,store_words});
    return scanout(r.source,plan);
}
bool NativeRaster::changed(const DisplayPlan& plan) {
    auto& r=*impl_; if (!r.initialized) return false;
    std::uint64_t key=1469598103934665603ULL;
    for (unsigned i=0;i<2;++i) if (plan.control&(1U<<i)) {
        const auto& s=plan.surfaces[i]; auto& image=r.display_surface(s.base,s.pitch,s.y+(plan.half_height ? (s.height+1)/2 : s.height));
        key=(key^image.revision)*1099511628211ULL;
    }
    const auto bytes=std::as_bytes(std::span(&plan,1)); for (auto b:bytes) key=(key^std::to_integer<unsigned>(b))*1099511628211ULL;
    if (key==r.last_present) return false; r.last_present=key; return true;
}
void NativeRaster::release_presentation() {
    auto& r=*impl_;
    // Owner has drained rendering and presentation before releasing images.
    for (auto [image,framebuffer]:r.present_framebuffers) vkDestroyFramebuffer(r.device,framebuffer,nullptr);
    r.present_framebuffers.clear();
    for (auto view:r.present_views) vkDestroyImageView(r.device,view,nullptr);
    r.present_views.clear();
    if (r.present_pipeline) vkDestroyPipeline(r.device,r.present_pipeline,nullptr);
    if (r.present_pass) vkDestroyRenderPass(r.device,r.present_pass,nullptr);
    r.present_pipeline={}; r.present_pass={}; r.last_present=0;
    r.field_count=0; r.previous_field=DisplayField::progressive; r.resolved_revision=0;
}

void NativeRaster::wait_submission(std::uint64_t token) {
    auto& r=*impl_;
    if (token<=r.completed) return;
    if (token>r.submitted) throw std::invalid_argument("Unknown native graphics submission");
    for (unsigned i=0;i<frame_slots;++i) if (r.slots[i].serial==token) {
        r.collect(i,true);
        return;
    }
    throw std::logic_error("Native graphics submission retired without completion");
}

std::uint64_t NativeRaster::present(const DisplayPlan& source_plan,VkImage target,VkFormat format,VkSemaphore acquired,VkSemaphore rendered) {
    auto& r=*impl_;
    auto plan=source_plan;
    Impl::Image* images[2]{};
    if (plan.field!=DisplayField::progressive) {
        images[0]=images[1]=&r.reconstruct(plan);
        plan.control=2; plan.half_height=0; plan.field=DisplayField::progressive;
        plan.surfaces[1]={0,0,0,0,plan.width,plan.height,0,0};
    } else {
        r.field_count=0; r.previous_field=DisplayField::progressive; r.resolved_revision=0;
        for (unsigned i=0;i<2;++i) if (plan.control&(1U<<i)) { const auto& s=plan.surfaces[i]; images[i]=&r.display_surface(s.base,s.pitch,s.y+(plan.half_height ? (s.height+1)/2 : s.height)); }
    }
    if (!images[0]) images[0]=images[1]; if (!images[1]) images[1]=images[0];
    if (!r.present_pass) {
        VkAttachmentDescription a{}; a.format=format; a.samples=VK_SAMPLE_COUNT_1_BIT; a.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE; a.storeOp=VK_ATTACHMENT_STORE_OP_STORE; a.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED; a.finalLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}; VkSubpassDescription sub{}; sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS; sub.colorAttachmentCount=1; sub.pColorAttachments=&ref;
        VkRenderPassCreateInfo pi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; pi.attachmentCount=1; pi.pAttachments=&a; pi.subpassCount=1; pi.pSubpasses=&sub;
        checked(vkCreateRenderPass(r.device,&pi,nullptr,&r.present_pass),"Create native presentation pass"); r.present_format=format;
        r.present_pipeline=r.make_pipeline(r.present_pass,r.present_layout,present_vertex,present_fragment,1,false);
    }
    if (r.present_format!=format) throw std::runtime_error("Native presentation format changed");
    auto& fb=r.present_framebuffers[target];
    if (!fb) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image=target; vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.format=format; vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VkImageView view{}; checked(vkCreateImageView(r.device,&vi,nullptr,&view),"Create swapchain view"); r.present_views.push_back(view);
        VkFramebufferCreateInfo f{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; f.renderPass=r.present_pass; f.attachmentCount=1; f.pAttachments=&view; f.width=plan.width; f.height=plan.height; f.layers=1;
        checked(vkCreateFramebuffer(r.device,&f,nullptr,&fb),"Create display framebuffer");
    }
    auto& set=r.present_sets[{images[0],images[1]}];
    if (!set) {
        set=r.allocate_set(r.present_descriptors); VkDescriptorImageInfo info[2]{{r.sampler,images[0]->view,VK_IMAGE_LAYOUT_GENERAL},{r.sampler,images[1]->view,VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet=set; w.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.descriptorCount=2; w.pImageInfo=info;
        vkUpdateDescriptorSets(r.device,1,&w,0,nullptr);
    }
    r.order(); VkRenderPassBeginInfo b{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; b.renderPass=r.present_pass; b.framebuffer=fb; b.renderArea.extent={plan.width,plan.height};
    vkCmdBeginRenderPass(r.command,&b,VK_SUBPASS_CONTENTS_INLINE); r.in_pass=true;
    vkCmdBindPipeline(r.command,VK_PIPELINE_BIND_POINT_GRAPHICS,r.present_pipeline);
    vkCmdBindDescriptorSets(r.command,VK_PIPELINE_BIND_POINT_GRAPHICS,r.present_layout,0,1,&set,0,nullptr);
    const VkViewport vp{0,0,float(plan.width),float(plan.height),0,1}; const VkRect2D scissor{{0,0},{plan.width,plan.height}};
    vkCmdSetViewport(r.command,0,1,&vp); vkCmdSetScissor(r.command,0,1,&scissor);
    vkCmdPushConstants(r.command,r.present_layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(plan),&plan); vkCmdDraw(r.command,3,1,0,0);
    r.flush(acquired,rendered,false);
    return r.submitted;
}
}
