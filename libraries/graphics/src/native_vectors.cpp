#include "native_vectors.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace nt::graphics {
namespace {
const std::uint32_t assembly_code[]=
#include "vector_assembly.comp.inc"
;
void checked(VkResult r,const char* operation) {
    if (r!=VK_SUCCESS) throw std::runtime_error(std::string(operation)+": Vulkan result "+std::to_string(r));
}
constexpr VkDeviceSize alignment=256;
constexpr VkDeviceSize host_slot_bytes=32*1024*1024,device_slot_bytes=16*1024*1024;
constexpr VkDeviceSize launch_output_bytes=1024*1024,resident_bytes=64*1024*1024;
constexpr VkDeviceSize parameter_bytes=156*4,plan_words=64+NativeVectors::record_words,plan_bytes=plan_words*4,status_bytes=4;
// Device buffer: VU0 and VU1 data memory views, per-slot element outputs, then
// the resident stream arena.
constexpr VkDeviceSize vu0_offset=0,vu1_offset=4096,outputs_offset=65536;
constexpr unsigned vu_rows[2]{256,1024};
VkDeviceSize aligned(VkDeviceSize value) { return (value+alignment-1)&~(alignment-1); }
// FNV-1a over the stream words. Hits are confirmed by comparing content.
std::uint64_t content_hash(std::span<const std::uint32_t> words) {
    std::uint64_t hash=0xcbf29ce484222325ULL^words.size();
    for (const auto word:words) { hash^=word; hash*=0x100000001b3ULL; }
    return hash;
}
void barrier(VkCommandBuffer command,VkPipelineStageFlags source_stages,VkAccessFlags source,
    VkPipelineStageFlags destination_stages,VkAccessFlags destination) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; b.srcAccessMask=source; b.dstAccessMask=destination;
    vkCmdPipelineBarrier(command,source_stages,destination_stages,0,1,&b,0,nullptr,0,nullptr);
}
}
struct NativeVectors::Impl {
    VkPhysicalDevice physical{}; VkDevice device{};
    unsigned slot_count=0;
    bool initialized=false;
    VkBuffer host{},device_buffer{}; VkDeviceMemory host_memory{},device_memory{};
    std::byte* host_mapping=nullptr;
    VkDeviceSize resident_offset=0,resident_used=0;
    // Device-local copy of the staging ring, uploaded once per slot before its
    // vector work: assembly reads host-record batches and plans from video memory.
    VkDeviceSize mirror_offset=0;
    VkDescriptorSetLayout element_descriptors{},assembly_descriptors{};
    VkPipelineLayout element_layout{},assembly_layout{};
    VkPipeline assembly{};
    VkDescriptorPool pool{};
    std::array<VkDescriptorSet,2> element_sets{};
    VkDescriptorSet assembly_set{};
    struct Kernel { VkPipeline pipeline{}; unsigned unit=0; };
    std::vector<Kernel> kernels;
    struct Resident { VkDeviceSize offset=0; std::vector<std::uint32_t> words; };
    std::unordered_map<std::uint64_t,std::vector<Resident>> resident;
    // table: host-record batches assembled by one dispatch when the slot is submitted.
    struct Slot { VkDeviceSize host_used=0,device_used=0; std::vector<VkDeviceSize> status; std::vector<std::uint32_t> table; std::uint32_t items=0; };
    std::vector<Slot> slots;
    Statistics stats;
    ~Impl() {
        for (const auto& kernel:kernels) vkDestroyPipeline(device,kernel.pipeline,nullptr);
        if (assembly) vkDestroyPipeline(device,assembly,nullptr);
        if (pool) vkDestroyDescriptorPool(device,pool,nullptr);
        if (element_layout) vkDestroyPipelineLayout(device,element_layout,nullptr);
        if (assembly_layout) vkDestroyPipelineLayout(device,assembly_layout,nullptr);
        if (element_descriptors) vkDestroyDescriptorSetLayout(device,element_descriptors,nullptr);
        if (assembly_descriptors) vkDestroyDescriptorSetLayout(device,assembly_descriptors,nullptr);
        if (host_mapping) vkUnmapMemory(device,host_memory);
        if (host) vkDestroyBuffer(device,host,nullptr);
        if (device_buffer) vkDestroyBuffer(device,device_buffer,nullptr);
        if (host_memory) vkFreeMemory(device,host_memory,nullptr);
        if (device_memory) vkFreeMemory(device,device_memory,nullptr);
    }
    void buffer(VkDeviceSize bytes,VkBufferUsageFlags usage,VkMemoryPropertyFlags flags,VkBuffer& result,VkDeviceMemory& memory) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size=bytes; bi.usage=usage;
        checked(vkCreateBuffer(device,&bi,nullptr,&result),"Create vector buffer");
        VkMemoryRequirements requirements{}; vkGetBufferMemoryRequirements(device,result,&requirements);
        VkPhysicalDeviceMemoryProperties properties{}; vkGetPhysicalDeviceMemoryProperties(physical,&properties);
        unsigned type=properties.memoryTypeCount;
        for (unsigned n=0;n<properties.memoryTypeCount;++n)
            if ((requirements.memoryTypeBits&(1U<<n)) && (properties.memoryTypes[n].propertyFlags&flags)==flags) { type=n; break; }
        if (type==properties.memoryTypeCount) throw std::runtime_error("No memory type for vector buffers");
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize=requirements.size; ai.memoryTypeIndex=type;
        checked(vkAllocateMemory(device,&ai,nullptr,&memory),"Allocate vector memory");
        checked(vkBindBufferMemory(device,result,memory,0),"Bind vector memory");
    }
    VkDescriptorSetLayout set_layout(std::span<const VkDescriptorType> types) {
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        for (unsigned n=0;n<types.size();++n) bindings.push_back({n,types[n],1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr});
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount=static_cast<std::uint32_t>(bindings.size()); li.pBindings=bindings.data();
        VkDescriptorSetLayout result{}; checked(vkCreateDescriptorSetLayout(device,&li,nullptr,&result),"Create vector descriptors");
        return result;
    }
    VkPipeline pipeline(std::span<const std::uint32_t> code,VkPipelineLayout layout) {
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; mi.codeSize=code.size_bytes(); mi.pCode=code.data();
        VkShaderModule module{}; checked(vkCreateShaderModule(device,&mi,nullptr,&module),"Create vector shader");
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; ci.layout=layout;
        ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,module,"main",nullptr};
        VkPipeline result{};
        const auto status=vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&ci,nullptr,&result);
        vkDestroyShaderModule(device,module,nullptr);
        checked(status,"Create vector pipeline");
        return result;
    }
    VkDeviceSize host_bytes(const VectorLaunch& launch) const {
        VkDeviceSize bytes=aligned(parameter_bytes)+aligned(plan_bytes)+aligned(status_bytes)+aligned(launch.prefix.size_bytes());
        for (const auto& write:launch.writes) bytes+=aligned(write.words.size_bytes());
        return bytes;
    }
    VkDeviceSize resident_bytes_of(const VectorLaunch& launch) const {
        VkDeviceSize bytes=0;
        for (const auto& write:launch.writes) if (write.resident) bytes+=aligned(write.words.size_bytes());
        return bytes;
    }
    VkDeviceSize output_bytes(const VectorLaunch& launch) const {
        return VkDeviceSize(launch.elements)*launch.element_words*4;
    }
};
NativeVectors::NativeVectors(VkPhysicalDevice physical,VkDevice device,VkBuffer records,VkDeviceSize record_bytes,unsigned slots):impl_(std::make_unique<Impl>()) {
    auto& v=*impl_; v.physical=physical; v.device=device; v.slot_count=slots; v.slots.resize(slots);
    VkPhysicalDeviceProperties properties{}; vkGetPhysicalDeviceProperties(physical,&properties);
    const auto& limits=properties.limits;
    if (limits.minStorageBufferOffsetAlignment>alignment || limits.maxDescriptorSetStorageBuffersDynamic<4 ||
        limits.maxStorageBufferRange<launch_output_bytes)
        throw std::runtime_error("Device cannot run GPU vector batches");
    if (limits.maxStorageBufferRange<outputs_offset+slots*device_slot_bytes+resident_bytes+slots*host_slot_bytes+launch_output_bytes)
        throw std::runtime_error("Device cannot run GPU vector batches");
    // Padded so a record binding's dynamic range at the end of a slot stays inside.
    v.buffer(slots*host_slot_bytes+launch_output_bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,v.host,v.host_memory);
    void* mapping=nullptr; checked(vkMapMemory(device,v.host_memory,0,VK_WHOLE_SIZE,0,&mapping),"Map vector staging");
    v.host_mapping=static_cast<std::byte*>(mapping);
    v.resident_offset=outputs_offset+slots*device_slot_bytes;
    v.mirror_offset=v.resident_offset+resident_bytes;
    v.buffer(v.mirror_offset+slots*host_slot_bytes+launch_output_bytes,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,v.device_buffer,v.device_memory);
    using T=VkDescriptorType;
    const T element_types[]{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC};
    const T assembly_types[]{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    v.element_descriptors=v.set_layout(element_types); v.assembly_descriptors=v.set_layout(assembly_types);
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pl.setLayoutCount=1;
    pl.pSetLayouts=&v.element_descriptors; checked(vkCreatePipelineLayout(device,&pl,nullptr,&v.element_layout),"Create element layout");
    pl.pSetLayouts=&v.assembly_descriptors; checked(vkCreatePipelineLayout(device,&pl,nullptr,&v.assembly_layout),"Create assembly layout");
    v.assembly=v.pipeline(assembly_code,v.assembly_layout);
    const std::array<VkDescriptorPoolSize,2> sizes{{{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,5},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC,5}}};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dp.maxSets=3; dp.poolSizeCount=2; dp.pPoolSizes=sizes.data();
    checked(vkCreateDescriptorPool(device,&dp,nullptr,&v.pool),"Create vector descriptor pool");
    const std::array<VkDescriptorSetLayout,3> layouts{v.element_descriptors,v.element_descriptors,v.assembly_descriptors};
    std::array<VkDescriptorSet,3> sets{};
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ai.descriptorPool=v.pool; ai.descriptorSetCount=3; ai.pSetLayouts=layouts.data();
    checked(vkAllocateDescriptorSets(device,&ai,sets.data()),"Allocate vector descriptors");
    v.element_sets={sets[0],sets[1]}; v.assembly_set=sets[2];
    // Assembly reads plans, tables and host records from the device mirror, and
    // kernel outputs from device memory: one binding covers the device buffer.
    const std::array<VkDescriptorBufferInfo,10> infos{{
        {v.device_buffer,vu0_offset,vu_rows[0]*16},{v.device_buffer,outputs_offset,launch_output_bytes},{v.host,0,parameter_bytes},
        {v.device_buffer,vu1_offset,vu_rows[1]*16},{v.device_buffer,outputs_offset,launch_output_bytes},{v.host,0,parameter_bytes},
        {v.device_buffer,0,VK_WHOLE_SIZE},{records,0,record_bytes},{v.device_buffer,v.mirror_offset,launch_output_bytes},
        {v.host,0,VK_WHOLE_SIZE}}};
    std::array<VkWriteDescriptorSet,10> writes{};
    for (unsigned n=0;n<10;++n) {
        const unsigned set=n<3 ? 0 : n<6 ? 1 : 2,binding=n<6 ? n%3 : n-6;
        writes[n]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; writes[n].dstSet=sets[set]; writes[n].dstBinding=binding;
        writes[n].descriptorCount=1; writes[n].pBufferInfo=&infos[n];
        writes[n].descriptorType=set<2 ? element_types[binding] : assembly_types[binding];
    }
    vkUpdateDescriptorSets(device,10,writes.data(),0,nullptr);
}
NativeVectors::~NativeVectors()=default;
unsigned NativeVectors::load_kernel(std::span<const std::uint32_t> spirv,unsigned unit) {
    if (unit>1 || spirv.size()<5 || spirv.size()>4*1024*1024 || spirv[0]!=0x07230203U)
        throw std::invalid_argument("Invalid element kernel SPIR-V or VU unit");
    auto& v=*impl_;
    v.kernels.push_back({v.pipeline(spirv,v.element_layout),unit});
    return static_cast<unsigned>(v.kernels.size()-1);
}
void NativeVectors::validate(const VectorLaunch& launch) const {
    const auto& v=*impl_;
    if (launch.kernel==vector_host_records) {
        if (!launch.elements || launch.elements>65536 || !launch.element_words || launch.element_words>65 ||
            !launch.writes.empty() || !launch.parameters.empty() || launch.range_guard ||
            launch.prefix.size()!=std::size_t(launch.elements)*launch.element_words)
            throw std::invalid_argument("Invalid host-record vector batch");
        for (std::size_t n=0;n<launch.prefix.size();n+=launch.element_words)
            if (launch.prefix[n]) throw std::invalid_argument("Host vector record reports unsupported behavior");
        if (v.output_bytes(launch)>launch_output_bytes || v.host_bytes(launch)>host_slot_bytes)
            throw std::invalid_argument("Host-record vector batch exceeds its bound");
        return;
    }
    if (launch.kernel>=v.kernels.size()) throw std::invalid_argument("Unknown vector element kernel");
    const auto rows=vu_rows[v.kernels[launch.kernel].unit];
    if (!launch.elements || launch.elements>65536 || !launch.element_words || launch.element_words>65 ||
        launch.parameters.size()!=156 || launch.prefix.size()%launch.element_words ||
        launch.prefix.size()/launch.element_words>launch.elements ||
        launch.parameters[0]!=launch.elements-launch.prefix.size()/launch.element_words ||
        launch.parameters[18]!=launch.prefix.size()/launch.element_words)
        throw std::invalid_argument("Invalid vector launch element range or parameters");
    for (std::size_t n=0;n<launch.prefix.size();n+=launch.element_words)
        if (launch.prefix[n]) throw std::invalid_argument("Vector startup record reports unsupported behavior");
    if (v.output_bytes(launch)>launch_output_bytes) throw std::invalid_argument("Vector launch output exceeds its bound");
    if (launch.range_guard) {
        switch (static_cast<VectorRangeStatus>(launch.range_guard(launch.parameters.data()))) {
        case VectorRangeStatus::valid: break;
        case VectorRangeStatus::count: throw std::invalid_argument("Vector loop count does not cover the requested elements");
        case VectorRangeStatus::startup: throw std::invalid_argument("Vector elements precede the recovered steady state");
        case VectorRangeStatus::wrap: throw std::invalid_argument("Vector loop integer or memory range wraps");
        case VectorRangeStatus::alias: throw std::invalid_argument("Vector loop input and output ranges alias");
        default: throw std::invalid_argument("Unknown vector range-guard status");
        }
    }
    for (const auto& write:launch.writes)
        if (write.words.empty() || write.words.size()%4 || write.row>=rows || write.words.size()/4>rows-write.row)
            throw std::invalid_argument("Vector memory write is outside VU data memory");
    if (v.host_bytes(launch)>host_slot_bytes || v.resident_bytes_of(launch)>resident_bytes)
        throw std::invalid_argument("Vector launch memory writes exceed their bound");
}
NativeVectors::Space NativeVectors::space(unsigned slot,const VectorLaunch& launch) const {
    const auto& v=*impl_; const auto& s=v.slots.at(slot);
    if (v.resident_used+v.resident_bytes_of(launch)>resident_bytes) return Space::resident_full;
    // Reserve the batch table written when the slot is submitted (one entry more).
    const VkDeviceSize table=aligned((s.table.size()+4+2)*sizeof(std::uint32_t));
    if (aligned(s.host_used)+v.host_bytes(launch)+table>host_slot_bytes || table>launch_output_bytes ||
        aligned(s.device_used)+aligned(v.output_bytes(launch))>device_slot_bytes) return Space::slot_full;
    return Space::fits;
}
void NativeVectors::record(VkCommandBuffer command,unsigned slot,const VectorLaunch& launch,const VectorAssembly& assembly,
    std::span<const std::uint32_t,record_words> primitive,unsigned first_record,unsigned primitives) {
    validate(launch);
    if (space(slot,launch)!=Space::fits) throw std::logic_error("Vector launch recorded without space");
    for (const auto& kind:assembly.store) for (const auto store:kind)
        if (store!=vector_constant_word && store>=launch.element_words-1) throw std::invalid_argument("Vector register names a missing store word");
    auto& v=*impl_; auto& s=v.slots.at(slot);
    const bool host_records=launch.kernel==vector_host_records;
    const unsigned unit=host_records ? 1U : v.kernels[launch.kernel].unit;
    const auto memory_offset=unit ? vu1_offset : vu0_offset;
    const auto host_base=slot*host_slot_bytes,device_base=outputs_offset+slot*device_slot_bytes;
    // The VU memory views start zeroed; only kernel launches read them.
    if (!host_records && !v.initialized) {
        vkCmdFillBuffer(command,v.device_buffer,0,vu1_offset+vu_rows[1]*16,0);
        v.initialized=true;
    }
    const auto host_allocate=[&](std::span<const std::byte> bytes) {
        const auto at=aligned(s.host_used); s.host_used=at+bytes.size();
        std::memcpy(v.host_mapping+host_base+at,bytes.data(),bytes.size());
        return host_base+at;
    };
    // Resolve resident streams and stage everything else.
    std::vector<VkBufferCopy> uploads;
    struct Write { bool from_host; VkBufferCopy copy; };
    std::vector<Write> writes;
    for (const auto& write:launch.writes) {
        const auto bytes=std::as_bytes(write.words);
        const VkDeviceSize destination=memory_offset+VkDeviceSize(write.row)*16;
        if (!write.resident) {
            writes.push_back({true,{host_allocate(bytes),destination,bytes.size()}});
            v.stats.staged_bytes+=bytes.size();
            continue;
        }
        auto& entries=v.resident[content_hash(write.words)];
        const auto hit=std::find_if(entries.begin(),entries.end(),[&](const auto& entry) {
            return std::equal(entry.words.begin(),entry.words.end(),write.words.begin(),write.words.end());
        });
        VkDeviceSize source=0;
        if (hit!=entries.end()) { source=hit->offset; ++v.stats.resident_hits; }
        else {
            source=v.resident_offset+v.resident_used; v.resident_used+=aligned(bytes.size());
            uploads.push_back({host_allocate(bytes),source,bytes.size()});
            entries.push_back({source,std::vector<std::uint32_t>(write.words.begin(),write.words.end())});
            ++v.stats.resident_uploads; v.stats.resident_upload_bytes+=bytes.size();
        }
        writes.push_back({false,{source,destination,bytes.size()}});
    }
    if (!uploads.empty()) vkCmdCopyBuffer(command,v.host,v.device_buffer,static_cast<std::uint32_t>(uploads.size()),uploads.data());
    // Uploads complete, and earlier kernels finish reading memory, before this launch's writes.
    if (!host_records) barrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT);
    for (std::size_t n=0;n<writes.size();++n) {
        const auto& copy=writes[n].copy;
        // Overlapping writes keep their order; independent copies may run together.
        const bool overlaps=std::any_of(writes.begin(),writes.begin()+static_cast<std::ptrdiff_t>(n),[&](const Write& earlier) {
            return earlier.copy.dstOffset<copy.dstOffset+copy.size && copy.dstOffset<earlier.copy.dstOffset+earlier.copy.size;
        });
        if (overlaps) barrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdCopyBuffer(command,writes[n].from_host ? v.host : v.device_buffer,v.device_buffer,1,&copy);
    }
    if (!host_records) barrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT);
    const auto parameters=host_records ? VkDeviceSize(0) : host_allocate(std::as_bytes(launch.parameters));
    const auto host_output=host_records ? host_allocate(std::as_bytes(launch.prefix)) : VkDeviceSize(0);
    const auto output=host_records ? VkDeviceSize(0) : device_base+aligned(s.device_used);
    if (!host_records) s.device_used=aligned(s.device_used)+v.output_bytes(launch);
    if (host_records) v.stats.staged_bytes+=launch.prefix.size_bytes();
    if (!host_records && !launch.prefix.empty()) {
        const VkBufferCopy copy{host_allocate(std::as_bytes(launch.prefix)),output,launch.prefix.size_bytes()};
        vkCmdCopyBuffer(command,v.host,v.device_buffer,1,&copy);
        v.stats.staged_bytes+=copy.size;
    }
    if (!host_records && launch.parameters[0]) {
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,v.kernels[launch.kernel].pipeline);
        const std::uint32_t element_offsets[]{static_cast<std::uint32_t>(output-outputs_offset),static_cast<std::uint32_t>(parameters)};
        vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,v.element_layout,0,1,&v.element_sets[unit],2,element_offsets);
        vkCmdDispatch(command,(launch.parameters[0]+63)/64,1,1);
    }
    if (!host_records) barrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT);
    std::array<std::uint32_t,plan_words> plan{};
    plan[0]=launch.elements; plan[1]=launch.element_words; plan[2]=first_record; plan[3]=assembly.primitive;
    plan[4]=assembly.shaded; plan[5]=assembly.coverage; plan[6]=assembly.xyz_register; plan[7]=assembly.present;
    plan[8]=static_cast<std::uint32_t>(assembly.offset_x); plan[9]=static_cast<std::uint32_t>(assembly.offset_y);
    std::copy(assembly.scissor.begin(),assembly.scissor.end(),plan.begin()+10);
    for (unsigned kind=0;kind<5;++kind) for (unsigned n=0;n<4;++n) {
        plan[14+kind*4+n]=assembly.store[kind][n]; plan[34+kind*4+n]=assembly.constant[kind][n];
    }
    std::copy(assembly.state.begin(),assembly.state.end(),plan.begin()+54);
    plan[60]=assembly.reglist;
    std::copy(primitive.begin(),primitive.end(),plan.begin()+64);
    const auto plan_offset=host_allocate(std::as_bytes(std::span(plan)));
    const std::uint32_t zero=0;
    const auto status=host_allocate(std::as_bytes(std::span(&zero,1)));
    s.status.push_back(status);
    // Word offsets into the device buffer: staging is mirrored at mirror_offset.
    const auto mirrored=[&](VkDeviceSize host_offset) { return static_cast<std::uint32_t>((v.mirror_offset+host_offset)/4); };
    const std::uint32_t items=std::max(launch.elements,primitives);
    const std::uint32_t record_word=host_records ? mirrored(host_output) : static_cast<std::uint32_t>(output/4);
    // Assembled by the slot's single dispatch, after its kernels and upload.
    s.table.insert(s.table.end(),{s.items,mirrored(plan_offset),record_word,static_cast<std::uint32_t>(status/4)});
    s.items+=items;
    // Batches write disjoint records. The owner's barrier after all vector work
    // makes records visible to draws and status words to the host.
    ++v.stats.launches;
}
bool NativeVectors::pending(unsigned slot) const { return impl_->slots.at(slot).host_used!=0; }
void NativeVectors::record_upload(VkCommandBuffer command,unsigned slot) {
    auto& v=*impl_; auto& s=v.slots.at(slot);
    if (!s.host_used) return;
    VkDeviceSize table_offset=0;
    if (!s.table.empty()) {
        std::vector<std::uint32_t> table;
        table.reserve(s.table.size()+2);
        table.push_back(static_cast<std::uint32_t>(s.table.size()/4));
        table.insert(table.end(),s.table.begin(),s.table.end());
        table.push_back(s.items);
        const auto bytes=std::as_bytes(std::span(table));
        if (bytes.size()>launch_output_bytes || aligned(s.host_used)+bytes.size()>host_slot_bytes)
            throw std::logic_error("Vector batch table exceeds its slot");
        table_offset=slot*host_slot_bytes+aligned(s.host_used); s.host_used=aligned(s.host_used)+bytes.size();
        std::memcpy(v.host_mapping+table_offset,bytes.data(),bytes.size());
    }
    const VkBufferCopy copy{slot*host_slot_bytes,v.mirror_offset+slot*host_slot_bytes,s.host_used};
    vkCmdCopyBuffer(command,v.host,v.device_buffer,1,&copy);
    // The mirror, and kernel outputs recorded earlier in this buffer, before assembly.
    barrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT);
    if (s.table.empty()) return;
    // One dispatch assembles every batch of the slot.
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,v.assembly);
    const auto offset=static_cast<std::uint32_t>(table_offset);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,v.assembly_layout,0,1,&v.assembly_set,1,&offset);
    vkCmdDispatch(command,(s.items+63)/64,1,1);
    barrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_HOST_BIT,
        VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_HOST_READ_BIT);
}
void NativeVectors::collect(unsigned slot) {
    auto& v=*impl_; auto& s=v.slots.at(slot);
    for (const auto offset:s.status) {
        std::uint32_t failures=0; std::memcpy(&failures,v.host_mapping+offset,sizeof(failures));
        v.stats.failures+=failures;
    }
    s={};
}
void NativeVectors::clear_resident() { impl_->resident.clear(); impl_->resident_used=0; }
const NativeVectors::Statistics& NativeVectors::statistics() const { return impl_->stats; }
}
