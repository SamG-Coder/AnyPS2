#include "native_raster.hpp"
#include <nt/palette_plan.hpp>
#include "native_vectors.hpp"
#include "raster_hazards.hpp"
#include "sprite_mapping.hpp"
#include <algorithm>
#include <bitset>
#include <chrono>
#include <cstring>
#include <map>
#include <optional>
#include <unordered_map>
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
const std::uint32_t palette_code[]=
#include "palette.comp.inc"
;
#ifdef ANYPS2_LLVM_TEXTURES
const std::uint32_t texture_decode_code[]=
#include "texture-decode.inc"
;
constexpr unsigned decode_sets_per_slot=64;
#endif
// An enabled NEVER alpha test always takes its failure action. FB_ONLY and
// RGB_ONLY cannot write depth. With an ALWAYS depth comparison they do not read
// it either: avoid creating dependencies on an unused image. Applies to CPU
// draws and vector batches alike.
void normalize_tests(Sprite& draw) {
    const auto failure=(draw.test>>12)&3U;
    if ((draw.test&15U)==1U && (failure==1 || failure==3)) draw.flags&=~depth_write_flag;
    if ((draw.test&15U)==1U && failure==2) draw.mask=0xffffffffU;
    if (((draw.test>>17)&3U)==1U && !(draw.flags&depth_write_flag)) draw.flags&=~depth_test_flag;
}
// Indexed sampling done by the fragment shader (native.frag): a GPU palette
// slot, its CSA bank, the format (0: none) and the levels it applies to.
// `host`, when set, is a CPU palette snapshot the draw record publishes.
struct IndexedSampling { std::uint32_t palette=0,bank=0,format=0,levels=0; const std::array<std::uint32_t,256>* host=nullptr; void* palette_surface=nullptr; void* texture_surface=nullptr; std::uint32_t texture_view=0; };
struct DrawRecord {
    Sprite draw; std::array<RasterVertex,3> vertices; unsigned width,height,fog_color,fog; std::uint32_t lod_control; float lod_q;
    std::uint32_t palette=0,palette_bank=0,indexed_format=0,indexed_levels=0;
    std::array<std::uint32_t,6> level_buffers{}; std::uint32_t dither[2]{};
};
static_assert(sizeof(DrawRecord)==320);
// Binding 0 holds level 0 followed by MIPTBP levels 1-6.
constexpr unsigned texture_level_slots=7;
// A submission holds a frame's worth of draws, so frames need few
// submissions and the host rarely waits to reuse a slot.
constexpr unsigned records_per_slot=16384;
// CPU palettes published per submission (one per draw at most).
constexpr unsigned host_palette_slots=16384;
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
// A mip level is the level-0 texture at its MIPTBP address with dimensions
// and region-clamp/repeat coordinates divided by 2^level.
SpriteCommand texture_level_command(const SpriteCommand& command,unsigned level) {
    auto result=command;
    auto& d=result.draw;
    d.texture_base=command.levels.base[level-1]; d.texture_pitch=command.levels.pitch[level-1];
    d.texture_width=std::max(1U,d.texture_width>>level); d.texture_height=std::max(1U,d.texture_height>>level);
    d.min_u>>=level; d.max_u>>=level; d.min_v>>=level; d.max_v>>=level;
    result.levels={};
    return result;
}
}
struct NativeRaster::Impl {
    VkPhysicalDevice physical; VkDevice device; VkQueue queue;
    bool ordered_fragments=false;
    VkQueryPool timestamps{}; double timestamp_period=0; unsigned timestamp_bits=0;
    // prologue holds vector batch compute work. It is submitted before command,
    // so a slot's draws run in render passes uninterrupted by that work.
    struct Submission { VkCommandBuffer command{},prologue{}; VkFence fence{}; std::uint64_t serial=0; bool measured=false,prologue_measured=false; };
    std::array<Submission,frame_slots> slots{};
    unsigned slot=0; std::uint64_t submitted=0,completed=0;
    VkCommandPool pool{}; VkCommandBuffer command{};
    VkBuffer staging{}; VkDeviceMemory staging_memory{}; void* mapped=nullptr;
    VkBuffer records{}; VkDeviceMemory record_memory{}; void* record_mapping=nullptr;
    // CPU palettes for shader-decoded draws: one 256-word entry per draw at
    // most, partitioned per frame slot like the draw records.
    VkBuffer host_palettes{}; VkDeviceMemory host_palette_memory{}; void* host_palette_mapping=nullptr;
    unsigned host_palettes_used=0; std::uint32_t last_host_palette=0; std::array<std::uint32_t,256> last_host_words{};
    std::uint32_t host_palette(const std::array<std::uint32_t,256>& words) {
        if (!host_palettes_used || last_host_words!=words) {
            last_host_palette=slot*host_palette_slots+host_palettes_used++; last_host_words=words;
            std::memcpy(static_cast<std::byte*>(host_palette_mapping)+std::size_t(last_host_palette)*1024,words.data(),1024);
        }
        return last_host_palette|0x80000000U;
    }
    // Words whose current value `source` holds (bit per word): set by host
    // writes and exports, cleared for every page a draw touches.
    std::vector<std::uint64_t> exact_words=std::vector<std::uint64_t>(store_words/64,~0ULL);
    bool exact_word(unsigned address) const { return (exact_words[address/64]>>(address%64))&1; }
    std::unique_ptr<NativeVectors> vectors;
    unsigned records_used=0,batch_first=0,batch_count=0,batch_vertices=0;
    VkPipeline batch_pipeline{},bound_pipeline{};
    VkDescriptorSet batch_set{},bound_set{};
    VkRenderPass render_pass{},present_pass{}; VkFormat present_format{};
    VkDescriptorSetLayout descriptors{},present_descriptors{}; VkDescriptorPool descriptor_pool{};
    VkPipelineLayout layout{},present_layout{}; VkPipeline pipeline{},coverage_pipeline{},present_pipeline{};
    VkDescriptorSetLayout field_descriptors{};
    VkPipelineLayout field_layout{}; VkPipeline field_pipeline{};
    // GPU palette slots (palette_slot_count x 256 words) followed by one
    // 256-word address list per slot for the gather pass, then a staging
    // area of (address, value, mask) entries for GPU image writes.
    static constexpr unsigned palette_slot_count=4096;
    static constexpr unsigned scatter_offset=palette_slot_count*2*256,scatter_capacity=262144;
    VkBuffer palettes{}; VkDeviceMemory palette_memory{};
    // Lazily updated GPU copy of `source`: a page is current when its
    // mirror revision equals its backing revision.
    VkBuffer mirror{}; VkDeviceMemory mirror_memory{};
    std::array<std::uint64_t,512> mirror_revisions{};
    VkDescriptorSetLayout palette_descriptors{}; VkPipelineLayout palette_layout{}; VkPipeline palette_pipeline{};
    std::uint32_t next_palette=1;
    unsigned resolution_scale=1;
    // Slot holding the last CPU palette snapshot uploaded for a GPU decode.
    std::uint32_t uploaded_palette=0; std::array<std::uint32_t,256> uploaded_snapshot{};
    struct PaletteParameters {
        std::uint32_t mode=0,surface_base=0,surface_pitch=0,surface_width=0,surface_height=0;
        std::uint32_t slot=0,addresses=0,count=0;
        std::uint32_t texture_base=0,texture_pitch=0,format=0,bank=0,width=0,height=0;
        std::array<std::uint32_t,16> owned{};
        std::uint32_t layout=0; // bit 0: surface in depth layout; bit 1: texture in depth layout
        std::uint32_t scale=1;
    };
    static_assert(sizeof(PaletteParameters)<=128);
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
    std::uint64_t submitted_passes=0,polled_passes=0; // render-pass counts at the last submission and idle poll
    std::optional<std::uint64_t> read_passes; // render-pass count at the last memory read
    // While a program keeps reading memory back, recorded work is handed to the GPU once
    // its earlier submissions have finished, so a read that must wait for the GPU finds
    // most of its frame already done. Without recent reads nothing waits on the GPU, and
    // each extra submission would only cost the render thread. Polled once per new pass.
    bool idle_submit_due() {
        constexpr std::uint64_t minimum_passes=2,read_window_passes=8192;
        if (!recording || !read_passes || stats.render_passes-*read_passes>read_window_passes) return false;
        if (stats.render_passes-submitted_passes<minimum_passes || stats.render_passes==polled_passes) return false;
        polled_passes=stats.render_passes;
        for (unsigned i=0;i<frame_slots;++i) {
            collect(i,false);
            if (slots[i].serial && !slots[i].measured) return false;
        }
        return true;
    }
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
        // width and height are console pixels; the image holds scale times as many per axis.
        unsigned width,height,base=0,pitch=0,format=0,scale=1; bool depth=false,dirty=false,valid=true,scratch=false;
        std::uint64_t revision=0,last_use=0,scope=0; Region region{};
        std::array<std::uint64_t,512> pages{};
        VkDeviceSize allocation_bytes=0;
        std::unique_ptr<Image> console_view;
        Image(VkDevice d,unsigned w,unsigned h):device(d),width(w),height(h) {}
        ~Image() {
            if (view) vkDestroyImageView(device,view,nullptr);
            if (image) vkDestroyImage(device,image,nullptr);
            if (memory) vkFreeMemory(device,memory,nullptr);
        }
    };
    std::map<std::tuple<unsigned,unsigned,unsigned>,std::unique_ptr<Image>> surfaces;
    std::uint64_t surface_use=0,surface_scope=1;
    // A palette whose words a rendered surface holds is read from that surface by
    // the shader, so loading it needs no GPU work and no end to the render pass. A
    // write to those words first gathers it, keeping the contents the load latched.
    struct SurfacePalette { std::uint32_t id=0; std::vector<unsigned> addresses; unsigned first=0; Image* holder=nullptr; };
    std::vector<SurfacePalette> surface_palettes;
    std::bitset<16384> surface_palette_blocks;
    SurfacePalette* surface_palette(std::uint32_t id);
    void gather_palette(std::uint32_t id,std::span<const unsigned> addresses,unsigned first);
    void gather_surface_palette(std::uint32_t id);
    void gather_surface_palettes();
    Image* sampled_surface(const Sprite&,Region);
    void prepare_write(const Sprite&,unsigned format);
    void prepare_write(Region);
    struct Texture {
        std::unique_ptr<Image> image; Region region; bool valid=false;
        std::uint64_t last_use=0,source_revision=0;
        std::uint32_t palette_slot=0,palette_bank=0; // GPU-decoded contents
        std::array<std::uint32_t,256> palette{};     // CPU-decoded indexed contents
    };
    // A texture view and, for CPU-decoded indexed data, a hash of its palette;
    // the palette itself is compared on a hit.
    struct TextureKey {
        std::array<std::uint32_t,6> view{}; std::uint64_t palette=0;
        bool operator==(const TextureKey&) const=default;
    };
    struct TextureKeyHash {
        std::size_t operator()(const TextureKey& key) const {
            std::uint64_t hash=0xcbf29ce484222325ULL^key.palette;
            for (const auto word:key.view) { hash^=word; hash*=0x100000001b3ULL; }
            return std::size_t(hash);
        }
    };
    static std::uint64_t palette_hash(const std::array<std::uint32_t,256>& words) {
        std::uint64_t hash=0xcbf29ce484222325ULL;
        for (const auto word:words) { hash^=word; hash*=0x100000001b3ULL; }
        return hash;
    }
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
    std::unordered_map<TextureKey,Texture,TextureKeyHash> textures;
    std::map<std::pair<Image*,Image*>,VkFramebuffer> framebuffers;
    std::map<std::array<Image*,2+texture_level_slots>,VkDescriptorSet> sets;
    std::map<std::pair<Image*,Image*>,VkDescriptorSet> present_sets;
    std::map<VkImage,VkFramebuffer> present_framebuffers;
    std::vector<VkImageView> present_views;
    std::array<std::unique_ptr<Image>,4> fields;
    std::unique_ptr<Image> display_image;
    std::map<std::tuple<Image*,Image*,unsigned>,VkDescriptorSet> field_sets;
    std::map<std::pair<Image*,Image*>,VkDescriptorSet> palette_sets;
    std::unique_ptr<Image> placeholder; // bound as the surface when only the mirror is read
    // field_count: consecutive alternating-parity fields; composed_count:
    // consecutive composed images in the ring, whatever their parity.
    unsigned field_index=0,field_count=0,composed_count=0;
    DisplayField previous_field=DisplayField::progressive;
    DisplayPlan resolved_plan{};
    std::uint64_t resolved_revision=0;
    std::array<std::uint64_t,2> resolved_sources{};
    RasterHazards hazards;
    VkFramebuffer current_framebuffer{};
    unsigned current_width=0,current_height=0;
    bool prologue_recording=false;
    // Surfaces of the previous vector batch, reused while they stay current.
    struct VectorTarget { Image* color=nullptr; Image* depth=nullptr; Sprite draw{}; };
    VectorTarget vector_target;
    VkCommandBuffer vector_commands();
    Impl(VkPhysicalDevice p,VkDevice d,VkQueue q,bool ordered):physical(p),device(d),queue(q),ordered_fragments(ordered) {}
    void initialize(unsigned family);
    ~Impl();
    VkDeviceMemory allocate(VkMemoryRequirements requirements,VkMemoryPropertyFlags flags,VkMemoryPropertyFlags preferred=0);
    void begin(); void end_pass(); void order();
    void flush_batch();
    void append_draw(VkDescriptorSet,const SpriteCommand&,IndexedSampling={});
    void collect(unsigned index,bool wait);
    void collect_vectors();
    void prepare_slot();
    void flush(VkSemaphore acquired=VK_NULL_HANDLE,VkSemaphore rendered=VK_NULL_HANDLE,bool wait=true);
    VkPipeline make_pipeline(VkRenderPass pass,VkPipelineLayout pipeline_layout,std::span<const std::uint32_t> vs,
        std::span<const std::uint32_t> fs,unsigned colors,bool coverage);
    std::unique_ptr<Image> image(unsigned width,unsigned height,unsigned scale=1);
    void upload_image(Image&,std::span<const std::uint32_t>);
    void copy_surface_pages(Image& destination,const Image& source,const std::bitset<512>& requested);
    void written(Image&,const Sprite&);
    bool current(const Image&,const Sprite&) const;
    // Publishes the surface's newer pages within `requested` to `source`.
    void blit_console_samples(const Image& source,Image& destination,unsigned first_row,unsigned last_row);
    void export_image(Image&,Region requested={0,store_words}); void synchronize_source(Region); void clear_views();
    void trim_surfaces();
    Image& surface(unsigned base,unsigned pitch,unsigned height,bool depth,bool scratch=false,bool display=false,const Sprite* draw=nullptr,unsigned format=0);
    Image& display_surface(unsigned base,unsigned pitch,unsigned height,unsigned format);
    std::uint64_t dither_matrix=0;
    // With `direct`, an indexed texture whose data and palette are on the GPU
    // is sampled by the draw shader: `direct` is filled and a placeholder returned.
    Image& texture(const SpriteCommand&,IndexedSampling* direct=nullptr);
    using TextureSet=std::array<Image*,texture_level_slots>;
    TextureSet textures_for(const SpriteCommand&,IndexedSampling* direct=nullptr);
    Image& reconstruct(const DisplayPlan&);
#ifdef ANYPS2_LLVM_TEXTURES
    void decode_texture(Image&,const SpriteCommand&,Region);
    void initialize_texture_decoder();
#endif
    VkDescriptorSet allocate_set(VkDescriptorSetLayout);
    void read_palette_words(std::uint32_t slot,std::array<std::uint32_t,256>& words);
    void palette_pass(Image& source,Image* output,const PaletteParameters&);
    bool gpu_source(Region requested,bool* rendered=nullptr);
    bool gpu_source_pages(const std::bitset<512>& pages,bool* rendered=nullptr);
    PaletteParameters surface_pages_pass(const Image&,const std::array<std::uint32_t,16>& owned,std::uint32_t mode) const;
    // Loads `pages` of a surface from the memory mirror without a CPU readback.
    // `whole` also loads pages that are current by revision, for an image without valid pixels.
    bool import_surface_pages(Image&,const std::bitset<512>& pages,bool whole=false);
    // Applies host image writes (already in `source`) to rendered surfaces.
    void scatter(std::span<const PixelWrite> writes,std::span<Image* const> targets);
    // Unique whole-word writes: (address, value) pairs, applied the same way.
    void scatter_words(std::span<const std::uint32_t> pairs,std::span<Image* const> targets);
    // Rendered views a host write over `range` updates on the GPU; others publish first.
    std::vector<Image*> write_targets(Region range);
    // Revision bookkeeping after host writes changed [first,last].
    void commit_write(unsigned first,unsigned last,std::span<Image* const> targets);
    std::vector<std::uint32_t> rect_pairs;
    // Uploads entries (address, value[, mask]) and applies them to targets.
    void scatter_entries(std::span<const std::uint32_t> entries,unsigned stride,std::span<Image* const> targets);
    void write_packed(const ImageRect& rect);
    std::vector<std::uint32_t> scatter_stamps,scatter_entry; std::uint32_t scatter_stamp=0;
    VkDescriptorSet bind_target(Image&,Image&,const TextureSet&,const Sprite&);
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
    if (timestamps) { vkCmdResetQueryPool(command,timestamps,slot*4,2); vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,timestamps,slot*4); }
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
void NativeRaster::Impl::append_draw(VkDescriptorSet set,const SpriteCommand& c,IndexedSampling indexed) {
    const auto selected=(c.draw.flags&coverage_flag) && (c.draw.flags&triangle_flag) ? coverage_pipeline : pipeline;
    const unsigned vertices=(c.draw.flags&triangle_flag) ? 3 : 6;
    if (batch_count && (set!=batch_set || selected!=batch_pipeline || vertices!=batch_vertices)) flush_batch();
    const unsigned index=slot*records_per_slot+records_used++;
    DrawRecord record{c.draw,c.triangle,current_width,current_height,c.fog_color,c.fog,c.levels.control,c.levels.q,
        indexed.host ? host_palette(*indexed.host) : indexed.palette,indexed.bank,indexed.format,indexed.levels};
    for (unsigned level=0;level<6;++level) record.level_buffers[level]=c.levels.base[level]|(c.levels.pitch[level]<<16);
    if (indexed.texture_surface) record.level_buffers[0]=indexed.texture_view;
    record.dither[0]=std::uint32_t(dither_matrix); record.dither[1]=std::uint32_t(dither_matrix>>32);
    std::memcpy(static_cast<std::byte*>(record_mapping)+index*sizeof(DrawRecord),&record,sizeof(record));
    if (!batch_count) { batch_first=index; batch_set=set; batch_pipeline=selected; batch_vertices=vertices; }
    ++batch_count;
}
void NativeRaster::Impl::end_pass() { if (in_pass) { flush_batch(); vkCmdEndRenderPass(command); in_pass=false; current_framebuffer={}; hazards.clear(); bound_set={}; bound_pipeline={}; } }
void NativeRaster::Impl::order() {
    end_pass(); begin(); VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; b.srcAccessMask=b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&b,0,nullptr,0,nullptr); ++stats.ordering_barriers;
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
        checked(vkGetQueryPoolResults(device,timestamps,index*4,2,sizeof(values),values,sizeof(values[0]),VK_QUERY_RESULT_64_BIT),"Read native GPU timestamps");
        const auto mask=timestamp_bits==64 ? ~std::uint64_t(0) : (std::uint64_t(1)<<timestamp_bits)-1;
        stats.gpu_ns+=static_cast<std::uint64_t>(double((values[1]-values[0])&mask)*timestamp_period);
        if (entry.prologue_measured) {
            checked(vkGetQueryPoolResults(device,timestamps,index*4+2,2,sizeof(values),values,sizeof(values[0]),VK_QUERY_RESULT_64_BIT),
                "Read native vector GPU timestamps");
            stats.vector_gpu_ns+=static_cast<std::uint64_t>(double((values[1]-values[0])&mask)*timestamp_period);
        }
    }
    if (vectors) { vectors->collect(index); collect_vectors(); }
    entry.measured=true;
}
void NativeRaster::Impl::collect_vectors() {
    const auto& v=vectors->statistics();
    stats.vector_launches=v.launches; stats.vector_resident_hits=v.resident_hits;
    stats.vector_resident_uploads=v.resident_uploads; stats.vector_resident_upload_bytes=v.resident_upload_bytes;
    stats.vector_staged_bytes=v.staged_bytes; stats.vector_failures=v.failures;
}
void NativeRaster::Impl::prepare_slot() {
    // A slot owns its command buffer and upload bytes until its fence completes.
    collect(slot,true); command=slots[slot].command;
#ifdef ANYPS2_LLVM_TEXTURES
    decodes_used=0;
#endif
}
VkCommandBuffer NativeRaster::Impl::vector_commands() {
    begin();
    auto& entry=slots[slot];
    if (!prologue_recording) {
        checked(vkResetCommandBuffer(entry.prologue,0),"Reset native vector commands");
        VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; b.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        checked(vkBeginCommandBuffer(entry.prologue,&b),"Begin native vector commands"); prologue_recording=true;
        if (timestamps) {
            vkCmdResetQueryPool(entry.prologue,timestamps,slot*4+2,2);
            vkCmdWriteTimestamp(entry.prologue,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,timestamps,slot*4+2);
        }
    }
    return entry.prologue;
}
void NativeRaster::Impl::flush(VkSemaphore acquired,VkSemaphore rendered,bool wait) {
    end_pass();
    if (recording) {
        auto& entry=slots[slot];
        if (timestamps) vkCmdWriteTimestamp(command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,timestamps,slot*4+1);
        checked(vkEndCommandBuffer(command),"End native commands"); checked(vkResetFences(device,1,&entry.fence),"Reset native fence");
        // Vector work runs first: kernel launches, then (recorded now, after all
        // staging writes) the staging upload and one assembly dispatch.
        std::array<VkCommandBuffer,2> buffers{};
        unsigned count=0;
        entry.prologue_measured=false;
        if (prologue_recording) {
            vectors->record_upload(entry.prologue,slot);
            if (timestamps) vkCmdWriteTimestamp(entry.prologue,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,timestamps,slot*4+3);
            checked(vkEndCommandBuffer(entry.prologue),"End native vector commands");
            buffers[count++]=entry.prologue; prologue_recording=false;
            entry.prologue_measured=timestamps!=VK_NULL_HANDLE;
        }
        buffers[count++]=command;
        VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO}; s.commandBufferCount=count; s.pCommandBuffers=buffers.data();
        VkPipelineStageFlags stage=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        if (acquired) { s.waitSemaphoreCount=1; s.pWaitSemaphores=&acquired; s.pWaitDstStageMask=&stage; s.signalSemaphoreCount=1; s.pSignalSemaphores=&rendered; }
        { Measure timing(stats.wait_ns,profiling); checked(vkQueueSubmit(queue,1,&s,entry.fence),"Submit native graphics"); }
        entry.serial=++submitted; entry.measured=false; submitted_passes=stats.render_passes;
        if (wait) collect(slot,true);
        recording=false; staging_used=0; records_used=0; host_palettes_used=0; ++stats.submissions;
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
    const VkSpecializationMapEntry scale_entry{0,0,sizeof(resolution_scale)};
    const VkSpecializationInfo fragment_constants{1,&scale_entry,sizeof(resolution_scale),&resolution_scale};
    stages[1].pSpecializationInfo=&fragment_constants;
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
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; qi.queryType=VK_QUERY_TYPE_TIMESTAMP; qi.queryCount=frame_slots*4;
        checked(vkCreateQueryPool(device,&qi,nullptr,&timestamps),"Create native GPU timestamps");
    }
    if (properties.limits.maxStorageBufferRange<record_bytes) throw std::runtime_error("Native draw records exceed storage buffer limit");
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pi.queueFamilyIndex=family;
    checked(vkCreateCommandPool(device,&pi,nullptr,&pool),"Create native command pool");
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool=pool; ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount=1;
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    for (auto& entry:slots) {
        checked(vkAllocateCommandBuffers(device,&ca,&entry.command),"Allocate native commands");
        checked(vkAllocateCommandBuffers(device,&ca,&entry.prologue),"Allocate native vector commands");
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
    // Draw records are written sequentially by the host and by vector assembly,
    // and read by every draw: prefer device-local memory the host can map.
    record_memory=allocate(requirements,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    checked(vkBindBufferMemory(device,records,record_memory,0),"Bind native draw records");
    checked(vkMapMemory(device,record_memory,0,record_bytes,0,&record_mapping),"Map native draw records");
    bi.size=VkDeviceSize(host_palette_slots)*frame_slots*1024;
    checked(vkCreateBuffer(device,&bi,nullptr,&host_palettes),"Create host palettes");
    vkGetBufferMemoryRequirements(device,host_palettes,&requirements);
    host_palette_memory=allocate(requirements,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    checked(vkBindBufferMemory(device,host_palettes,host_palette_memory,0),"Bind host palettes");
    checked(vkMapMemory(device,host_palette_memory,0,bi.size,0,&host_palette_mapping),"Map host palettes");
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; si.magFilter=si.minFilter=VK_FILTER_NEAREST;
    si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST; si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; si.maxLod=0;
    checked(vkCreateSampler(device,&si,nullptr,&sampler),"Create native sampler");
    std::array<VkDescriptorSetLayoutBinding,7> bindings{{{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,texture_level_slots,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {1,VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},{2,VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {3,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {4,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},{5,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {6,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}}};
    if (ordered_fragments) bindings[1].descriptorType=bindings[2].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dl.bindingCount=7; dl.pBindings=bindings.data();
    checked(vkCreateDescriptorSetLayout(device,&dl,nullptr,&descriptors),"Create native descriptors");
    const VkDescriptorSetLayoutBinding pb{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}; dl.bindingCount=1; dl.pBindings=&pb;
    checked(vkCreateDescriptorSetLayout(device,&dl,nullptr,&present_descriptors),"Create display descriptors");
    std::array<VkDescriptorPoolSize,4> sizes{{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,65536*texture_level_slots},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,65536},{VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,65536},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,196608}}};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dp.maxSets=65536; dp.poolSizeCount=4; dp.pPoolSizes=sizes.data();
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
    {
        VkBufferCreateInfo pb{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        pb.size=(VkDeviceSize(scatter_offset)+VkDeviceSize(scatter_capacity)*3)*sizeof(std::uint32_t);
        pb.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        checked(vkCreateBuffer(device,&pb,nullptr,&palettes),"Create GPU palettes");
        VkMemoryRequirements requirements{}; vkGetBufferMemoryRequirements(device,palettes,&requirements);
        palette_memory=allocate(requirements,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        checked(vkBindBufferMemory(device,palettes,palette_memory,0),"Bind GPU palettes");
        pb.size=VkDeviceSize(store_words)*sizeof(std::uint32_t);
        pb.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        checked(vkCreateBuffer(device,&pb,nullptr,&mirror),"Create GPU memory mirror");
        vkGetBufferMemoryRequirements(device,mirror,&requirements);
        mirror_memory=allocate(requirements,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        checked(vkBindBufferMemory(device,mirror,mirror_memory,0),"Bind GPU memory mirror");
        mirror_revisions.fill(~0ULL);
        const std::array<VkDescriptorSetLayoutBinding,4> palette_bindings{{
            {0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            {1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            {2,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
            {3,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}}};
        dl.bindingCount=palette_bindings.size(); dl.pBindings=palette_bindings.data();
        checked(vkCreateDescriptorSetLayout(device,&dl,nullptr,&palette_descriptors),"Create palette descriptors");
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(PaletteParameters)};
        VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layout_info.setLayoutCount=1; layout_info.pSetLayouts=&palette_descriptors;
        layout_info.pushConstantRangeCount=1; layout_info.pPushConstantRanges=&range;
        checked(vkCreatePipelineLayout(device,&layout_info,nullptr,&palette_layout),"Create palette layout");
        VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        module_info.codeSize=sizeof(palette_code); module_info.pCode=palette_code;
        VkShaderModule module{}; checked(vkCreateShaderModule(device,&module_info,nullptr,&module),"Create palette shader");
        VkComputePipelineCreateInfo create{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        create.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,module,"main",nullptr};
        create.layout=palette_layout;
        const auto result=vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&create,nullptr,&palette_pipeline);
        vkDestroyShaderModule(device,module,nullptr);
        checked(result,"Create palette pipeline");
    }
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
    if (palette_pipeline) vkDestroyPipeline(device,palette_pipeline,nullptr);
    if (palette_layout) vkDestroyPipelineLayout(device,palette_layout,nullptr);
    if (palette_descriptors) vkDestroyDescriptorSetLayout(device,palette_descriptors,nullptr);
    if (palettes) vkDestroyBuffer(device,palettes,nullptr);
    if (mirror) vkDestroyBuffer(device,mirror,nullptr);
    if (mirror_memory) vkFreeMemory(device,mirror_memory,nullptr);
    if (palette_memory) vkFreeMemory(device,palette_memory,nullptr);
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
    vectors.reset();
    if (record_mapping) vkUnmapMemory(device,record_memory);
    if (host_palette_mapping) vkUnmapMemory(device,host_palette_memory);
    if (host_palettes) vkDestroyBuffer(device,host_palettes,nullptr);
    if (host_palette_memory) vkFreeMemory(device,host_palette_memory,nullptr);
    if (records) vkDestroyBuffer(device,records,nullptr);
    if (record_memory) vkFreeMemory(device,record_memory,nullptr);
    for (auto& entry:slots) if (entry.fence) vkDestroyFence(device,entry.fence,nullptr);
    if (pool) vkDestroyCommandPool(device,pool,nullptr);
    if (timestamps) vkDestroyQueryPool(device,timestamps,nullptr);
}
std::unique_ptr<NativeRaster::Impl::Image> NativeRaster::Impl::image(unsigned width,unsigned height,unsigned scale) {
    auto i=std::make_unique<Image>(device,width,height); i->scale=scale;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ci.imageType=VK_IMAGE_TYPE_2D; ci.format=VK_FORMAT_R32_UINT; ci.extent={width*scale,height*scale,1}; ci.mipLevels=ci.arrayLayers=1; ci.samples=VK_SAMPLE_COUNT_1_BIT; ci.tiling=VK_IMAGE_TILING_OPTIMAL;
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
    if (image.scale>1) {
        // Every sample of a pixel takes its value, in bands that fit the staging arena.
        const std::size_t row_bytes=std::size_t(image.width)*image.scale*image.scale*4;
        const unsigned band=unsigned(std::max<VkDeviceSize>(1,staging_bytes/row_bytes));
        for (unsigned y=0;y<image.height;y+=band) {
            const unsigned rows=std::min(band,image.height-y);
            const std::size_t bytes=row_bytes*rows;
            if (bytes>staging_bytes-staging_used) flush();
            prepare_slot();
            const auto offset=slot*staging_bytes+staging_used;
            auto* out=reinterpret_cast<std::uint32_t*>(static_cast<std::byte*>(mapped)+offset);
            for (unsigned row=0;row<rows;++row) for (unsigned sy=0;sy<image.scale;++sy) for (unsigned x=0;x<image.width;++x)
                for (unsigned sx=0;sx<image.scale;++sx) *out++=pixels[std::size_t(y+row)*image.width+x];
            staging_used+=bytes; order();
            VkBufferImageCopy copy{}; copy.bufferOffset=offset; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
            copy.imageOffset={0,int(y*image.scale),0}; copy.imageExtent={image.width*image.scale,rows*image.scale,1};
            vkCmdCopyBufferToImage(command,staging,image.image,VK_IMAGE_LAYOUT_GENERAL,1,&copy); stats.upload_bytes+=bytes;
        }
        image.valid=true;
        return;
    }
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
// Nearest filtering of an odd scale lands on each pixel's center sample.
void NativeRaster::Impl::blit_console_samples(const Image& source,Image& destination,unsigned first_row,unsigned last_row) {
    order();
    const auto width=std::min(source.width,destination.width);
    VkImageBlit region{};
    region.srcSubresource=region.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    region.srcOffsets[0]={0,int(first_row*source.scale),0}; region.dstOffsets[0]={0,int(first_row),0};
    region.srcOffsets[1]={int(width*source.scale),int(last_row*source.scale),1}; region.dstOffsets[1]={int(width),int(last_row),1};
    vkCmdBlitImage(command,source.image,VK_IMAGE_LAYOUT_GENERAL,destination.image,VK_IMAGE_LAYOUT_GENERAL,1,&region,VK_FILTER_NEAREST);
    order();
}
void NativeRaster::Impl::export_image(Image& image,Region requested) {
    if (!image.dirty || image.scratch) return;
    std::bitset<512> newer,wanted;
    for (unsigned word=requested.start,left=requested.length;left;) {
        wanted.set((word/2048)%512);
        const unsigned step=std::min(left,2048-word%2048); word=(word+step)%store_words; left-=step;
    }
    bool remaining=false;
    for (unsigned page=0;page<512;++page) if (image.pages[page]>backing_revisions[page]) {
        if (wanted[page]) newer.set(page); else remaining=true;
    }
    if (newer.none()) { if (!remaining) image.dirty=false; return; }
    Measure timing(stats.surface_export_ns,profiling);
    // Only the rows holding the requested pages are read back (pages are
    // 64x32-pixel tiles of a page-aligned 32-bit surface).
    unsigned y0=0,y1=image.height;
    if (image.base%32==0 && !image.format) {
        y0=image.height; y1=0;
        for (unsigned page=0;page<512;++page) if (newer[page]) {
            const unsigned row=((page+512-image.base/32)%512)/image.pitch*32;
            if (row>=image.height) continue;
            y0=std::min(y0,row); y1=std::max(y1,std::min(row+32,image.height));
        }
        if (y1<=y0) { y0=0; y1=image.height; }
    }
    order();
    // A scaled surface keeps a console-sized view for repeated reads of its center
    // samples, and each read resolves only the rows it needs.
    if (image.scale>1) {
        if (!image.console_view) image.console_view=this->image(image.width,image.height);
        blit_console_samples(image,*image.console_view,y0,y1);
    }
    VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    copy.imageOffset={0,int(y0),0}; copy.imageExtent={image.width,y1-y0,1};
    vkCmdCopyImageToBuffer(command,image.console_view ? image.console_view->image : image.image,VK_IMAGE_LAYOUT_GENERAL,readback,1,&copy); flush();
    // Host-visible staging may be uncached/write-combined. Read it once with a
    // contiguous copy before CPU address conversion instead of scalar loads
    // directly from the mapped allocation for every pixel.
    std::vector<std::uint32_t> pixels(std::size_t(image.width)*(y1-y0));
    std::memcpy(pixels.data(),readback_mapping,pixels.size()*sizeof(pixels[0]));
    for (unsigned y=y0;y<y1;++y) for (unsigned x=0;x<image.width;++x) {
        const auto half=image.format ? rgba_halfword(image.base,image.pitch,x,y,image.format==10) : 0;
        const auto address=image.format ? half/2 : rgba_word(image.base,image.pitch,x,y)^(image.depth ? 0x600 : 0);
        if (newer[address/2048]) {
            const auto pixel=pixels[std::size_t(y-y0)*image.width+x];
            if (image.format) {
                const auto shift=(half%2)*16;
                source[address]=(source[address]&~(65535U<<shift))|(pack_rgba16(pixel)<<shift);
            } else source[address]=pixel;
        }
    }
    for (unsigned page=0;page<512;++page) if (newer[page]) {
        backing_revisions[page]=image.pages[page];
        std::fill_n(exact_words.begin()+page*32,32,~0ULL);
    }
    stats.readback_bytes+=pixels.size()*4;
    if (remaining) return;
    // Once the writer becomes clean, surface() can no longer discover it via
    // dirty dependencies. Invalidate older aliases before retiring that link.
    for (auto& [key,other]:surfaces)
        if (other && other.get()!=&image && other->revision<image.revision && other->region.overlaps(image.region))
            other->valid=false;
    image.dirty=false;
}
void NativeRaster::Impl::synchronize_source(Region requested) {
    std::vector<Image*> ordered;
    for (auto& [key,i]:surfaces) if (i && i->dirty && i->region.overlaps(requested)) ordered.push_back(i.get());
    std::sort(ordered.begin(),ordered.end(),[](auto a,auto b){return a->revision<b->revision;});
    for (auto* i:ordered) export_image(*i,requested);
}
void NativeRaster::Impl::clear_views() {
    vector_target={};
    flush(); for (auto [k,f]:framebuffers) vkDestroyFramebuffer(device,f,nullptr); framebuffers.clear(); sets.clear(); present_sets.clear();
    field_sets.clear(); palette_sets.clear();
    checked(vkResetDescriptorPool(device,descriptor_pool,0),"Reset native resource descriptors");
}
void NativeRaster::Impl::trim_surfaces() {
    // Retire descriptors and queued users before releasing an image. Keep the
    // current draw/display's acquired views alive while resolving its remaining
    // resources. Dirty pages must reach backing storage before their owner goes.
    gather_surface_palettes();
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
            copy.srcOffset={int(sx*source.scale),int(sy*source.scale),0}; copy.dstOffset={int(x*destination.scale),int(y*destination.scale),0};
            copy.extent={tile_width*source.scale,tile_height*source.scale,1};
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
            std::fill_n(exact_words.begin()+page*32,32,0ULL);
        }
    } else for (unsigned y=draw.top/(image.format ? 64:32);y<=(draw.bottom-1)/(image.format ? 64:32);++y)
        for (unsigned x=draw.left/64;x<=(draw.right-1)/64;++x) {
            const auto page=(image.base/32+y*image.pitch+x)&511;
            image.pages[page]=source_revisions[page]=revision;
            std::fill_n(exact_words.begin()+page*32,32,0ULL);
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
    if (slot && slot->height<height) { gather_surface_palettes(); export_image(*slot); clear_views(); slot.reset(); }
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
        slot=image(width,height,resolution_scale);
        slot->base=base; slot->pitch=pitch; slot->depth=depth; slot->scratch=scratch; slot->format=format;
        slot->region=scratch ? Region{} : requested; slot->valid=false;
        slot->last_use=++surface_use; slot->scope=surface_scope;
    }
    bool imported=false;
    for (auto& [other_key,other]:surfaces) if (other && other.get()!=slot.get() && other->dirty && other->region.overlaps(slot->region) &&
        [&] { for (unsigned page=0;page<512;++page)
            if (requested_pages[page] && other->pages[page]>slot->pages[page]) return true;
            return false; }()) {
        ++stats.surface_conflicts;
        stats.last_surface_conflict={slot->base,slot->pitch,slot->height,unsigned(slot->depth),
            other->base,other->pitch,other->height,unsigned(other->depth)};
        // Only a CPU consumer needs backing memory: another view takes these
        // pages through the GPU mirror, so the game thread never waits on them.
        if (!compose) {
            if (slot->valid && import_surface_pages(*slot,requested_pages)) imported=true;
            else slot->valid=false;
        }
        break;
    }
    bool changed=!slot->valid || imported;
    if (!slot->valid && !scratch && !compose) {
        std::bitset<512> pages;
        for (unsigned page=0;page<512;++page) if (slot->region.overlaps({page*2048,2048})) pages.set(page);
        if (import_surface_pages(*slot,pages,true)) slot->valid=true;
        else synchronize_source(slot->region);
    }
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
NativeRaster::Impl::Image& NativeRaster::Impl::display_surface(unsigned base,unsigned pitch,unsigned height,unsigned format) {
    const auto found=surfaces.find({base,pitch,format});
    if (found!=surfaces.end() && found->second && found->second->height>=height)
        return surface(base,pitch,height,false,false,false,nullptr,format);
    // Display offsets can read beyond the drawing extent, even into adjacent
    // storage. Such a read must not permanently enlarge the writable target:
    // that would make later unrelated draws conflict through the extra rows.
    // This separately cached view is never dirtied by drawing. Page-aligned
    // aliases are composed on the GPU; other layouts synchronize backing data.
    return surface(base,pitch,height,false,false,true,nullptr,format);
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
// Makes the GPU memory mirror current for `requested`. Pages whose newest
// words are in backing memory are uploaded; pages a rendered 32-bit surface
// holds are published from it by a GPU pass, so no readback is needed.
// `rendered` reports whether any page is newer than backing memory. Fails
// when no plain 32-bit surface holds the newest words of a page.
bool NativeRaster::Impl::gpu_source(Region requested,bool* rendered) {
    std::bitset<512> pages;
    for (unsigned word=requested.start,left=requested.length;left;) {
        pages.set((word/2048)%512);
        const unsigned step=std::min(left,2048-word%2048); word=(word+step)%store_words; left-=step;
    }
    return gpu_source_pages(pages,rendered);
}
bool NativeRaster::Impl::gpu_source_pages(const std::bitset<512>& pages,bool* rendered) {
    std::vector<std::pair<Image*,std::array<std::uint32_t,16>>> publish;
    bool newer=false;
    for (unsigned page=0;page<512;++page) if (pages[page]) {
        const auto latest=source_revisions[page];
        if (backing_revisions[page]<latest) {
            Image* holder=nullptr;
            for (auto& [key,image]:surfaces)
                if (image && image->dirty && image->valid && !image->scratch && (!image->format || image->format==2 || image->format==10) &&
                    image->pages[page]==latest) {
                    holder=image.get(); break;
                }
            if (!holder) return false;
            newer=true;
            if (mirror_revisions[page]==latest) continue;
            auto found=std::find_if(publish.begin(),publish.end(),[&](const auto& entry){return entry.first==holder;});
            if (found==publish.end()) found=publish.insert(publish.end(),{holder,{}});
            found->second[page/32]|=1U<<(page%32);
            continue;
        }
        if (mirror_revisions[page]==latest) continue;
        // Same staging rule as upload_image: transfers keep their bytes until the fence.
        if (2048*4>staging_bytes-staging_used) flush();
        prepare_slot();
        const auto offset=slot*staging_bytes+staging_used;
        std::memcpy(static_cast<std::byte*>(mapped)+offset,source.data()+std::size_t(page)*2048,2048*4);
        staging_used+=2048*4; order();
        const VkBufferCopy copy{offset,VkDeviceSize(page)*2048*4,2048*4};
        vkCmdCopyBuffer(command,staging,mirror,1,&copy);
        mirror_revisions[page]=latest; stats.upload_bytes+=2048*4;
    }
    for (const auto& [holder,owned]:publish) {
        palette_pass(*holder,nullptr,surface_pages_pass(*holder,owned,3));
        for (unsigned page=0;page<512;++page) if (owned[page/32]&(1U<<(page%32))) mirror_revisions[page]=source_revisions[page];
    }
    if (rendered) *rendered=newer;
    return true;
}
NativeRaster::Impl::PaletteParameters NativeRaster::Impl::surface_pages_pass(const Image& surface,const std::array<std::uint32_t,16>& owned,std::uint32_t mode) const {
    PaletteParameters parameters{};
    parameters.mode=mode; parameters.surface_base=surface.base; parameters.surface_pitch=surface.pitch;
    parameters.surface_width=surface.width; parameters.surface_height=surface.height;
    parameters.width=surface.width; parameters.height=surface.height;
    if (surface.base%32==0) {
        // Pages are 64x32-pixel tiles of a 32-bit image (64x64 of a
        // 16-bit one): cover only the owned ones.
        const unsigned page_rows=surface.format ? 64 : 32;
        unsigned x0=surface.width,y0=surface.height,x1=0,y1=0;
        for (unsigned page=0;page<512;++page) if (owned[page/32]&(1U<<(page%32))) {
            const unsigned relative=(page+512-surface.base/32)%512,x=(relative%surface.pitch)*64,y=(relative/surface.pitch)*page_rows;
            if (x>=surface.width || y>=surface.height) continue;
            x0=std::min(x0,x); y0=std::min(y0,y); x1=std::max(x1,x+64); y1=std::max(y1,std::min(y+page_rows,surface.height));
        }
        if (x1>x0 && y1>y0) { parameters.texture_base=x0; parameters.texture_pitch=y0; parameters.width=x1-x0; parameters.height=y1-y0; }
    }
    parameters.owned=owned;
    parameters.layout=(surface.depth ? 1U : 0U)|(surface.format ? 4U : 0U)|(surface.format==10 ? 8U : 0U);
    return parameters;
}
// The imported pages stay dirty in their writers until those export, so the
// surface's own dirty state is unchanged: clearing it could lose its newer pages.
bool NativeRaster::Impl::import_surface_pages(Image& surface,const std::bitset<512>& pages,bool whole) {
    std::bitset<512> stale;
    for (unsigned page=0;page<512;++page)
        if (pages[page] && (whole || surface.pages[page]!=source_revisions[page])) stale.set(page);
    if (stale.none()) return true;
    if (!gpu_source_pages(stale)) return false;
    std::array<std::uint32_t,16> owned{};
    for (unsigned page=0;page<512;++page) if (stale[page]) owned[page/32]|=1U<<(page%32);
    palette_pass(surface,nullptr,surface_pages_pass(surface,owned,5));
    for (unsigned page=0;page<512;++page) if (stale[page]) surface.pages[page]=source_revisions[page];
    ++stats.surface_gpu_imports;
    return true;
}
void NativeRaster::Impl::palette_pass(Image& source,Image* output,const PaletteParameters& parameters) {
    if (palette_sets.size()>=4096) clear_views();
    auto& cached=palette_sets[{&source,output}];
    if (!cached) {
    cached=allocate_set(palette_descriptors);
    const auto set=cached;
    const VkDescriptorImageInfo surface_info{sampler,source.view,VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorBufferInfo buffer_info{palettes,0,VK_WHOLE_SIZE};
    const VkDescriptorImageInfo output_info{VK_NULL_HANDLE,(output ? output : &source)->view,VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorBufferInfo mirror_info{mirror,0,VK_WHOLE_SIZE};
    std::array<VkWriteDescriptorSet,4> writes{};
    for (unsigned i=0;i<4;++i) {
        auto& w=writes[i]; w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet=set; w.dstBinding=i; w.descriptorCount=1;
    }
    writes[0].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[0].pImageInfo=&surface_info;
    writes[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[1].pBufferInfo=&buffer_info;
    writes[2].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[2].pImageInfo=&output_info;
    writes[3].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[3].pBufferInfo=&mirror_info;
    vkUpdateDescriptorSets(device,writes.size(),writes.data(),0,nullptr);
    }
    const auto set=cached;
    order();
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,palette_pipeline);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,palette_layout,0,1,&set,0,nullptr);
    auto scaled=parameters; scaled.scale=source.scale;
    vkCmdPushConstants(command,palette_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(scaled),&scaled);
    if (parameters.mode==0 || parameters.mode==2 || parameters.mode==4) vkCmdDispatch(command,(parameters.count+7)/8,1,1);
    else vkCmdDispatch(command,(parameters.width+7)/8,(parameters.height+7)/8,1);
    order();
}
NativeRaster::Impl::Image& NativeRaster::Impl::texture(const SpriteCommand& command,IndexedSampling* direct) {
    if (command.draw.flags&texture_16_flag) {
        // The shader reads 16-bit texels from the memory mirror at their own layout.
        const auto& s=command.draw;
        if (!direct) throw std::runtime_error("16-bit textures require shader sampling");
        const auto rows=texture_extent(s.texture_height,s.clamp_v,s.min_v,s.max_v);
        const auto source=region(s.texture_base,s.texture_pitch,texture_extent(s.texture_width,s.clamp_u,s.min_u,s.max_u),(rows+1)/2);
        if (!gpu_source(source)) { ++stats.texture_readbacks; synchronize_source(source); }
        direct->format=(s.flags&texture_16s_flag) ? 9U : 8U;
        if (!placeholder) placeholder=image(1,1);
        ++stats.shader_indexed_draws;
        return *placeholder;
    }
    Measure timing(stats.texture_lookup_ns,profiling);
    const auto& d=command.draw;
    if (d.flags&64) {
        unsigned width=texture_extent(d.texture_width,d.clamp_u,d.min_u,d.max_u);
        unsigned height=texture_extent(d.texture_height,d.clamp_v,d.min_v,d.max_v);
        const bool high=(d.flags&texture_high_flag)!=0;
        const auto r=region(d.texture_base,d.texture_pitch,width,height,high ? 0U : (d.flags&indexed8_flag) ? 2U : 1U);
        // Read in place needs the shader path, a free level binding and a surface
        // the draw does not render to; anything else gathers the palette first.
        const auto* in_place=surface_palette(command.palette_slot);
        if (in_place && (!direct || command.levels.control || (in_place->holder->base==d.base && in_place->holder->pitch==d.pitch))) {
            gather_surface_palette(command.palette_slot); in_place=nullptr;
        }
        // Indexed data decodes on the GPU when its palette is GPU-resident or
        // its index data was rendered: words come from the GPU memory mirror,
        // so neither needs a readback. A CPU palette is uploaded into a slot,
        // reused while its snapshot is unchanged.
        const bool rendered=std::any_of(surfaces.begin(),surfaces.end(),[&](const auto& entry) {
            return entry.second && entry.second->dirty && entry.second->region.overlaps(r);
        });
        if ((command.palette_slot || command.shader_palette || rendered) && gpu_source(r)) {
            std::uint32_t slot=command.palette_slot,bank=command.palette_bank;
            if (direct && !command.levels.control && !high)
                if (auto* holder=sampled_surface(d,r)) { direct->texture_surface=holder; direct->texture_view=holder->base|(holder->pitch<<16); }
            if (!slot && direct) {
                // The snapshot already applied CSA; the draw record publishes it.
                direct->host=&command.palette; direct->bank=0;
                direct->format=1+(high ? ((d.flags&indexed8_flag) ? 2U : (d.flags&texture_high_nibble_flag) ? 4U : 3U) : (d.flags&indexed8_flag) ? 0U : 1U);
                if (!placeholder) placeholder=image(1,1);
                ++stats.shader_indexed_draws;
                return *placeholder;
            }
            if (!slot) {
                if (!uploaded_palette || uploaded_snapshot!=command.palette) {
                    uploaded_palette=next_palette++; uploaded_snapshot=command.palette;
                    order();
                    vkCmdUpdateBuffer(this->command,palettes,VkDeviceSize(uploaded_palette%palette_slot_count)*256*4,
                        sizeof(uploaded_snapshot),uploaded_snapshot.data());
                }
                slot=uploaded_palette; bank=0; // the snapshot already applied CSA
            }
            if (direct) {
                if (in_place) {
                    const auto* holder=in_place->holder;
                    const unsigned palette_base=(in_place->addresses[0]-((in_place->first&1)+((in_place->first&6)<<1)+((in_place->first&8)>>2)+
                        ((in_place->first&16)<<2)+((in_place->first&96)>>1)+(in_place->first&128)))/64;
                    direct->palette=0x40000000U|palette_base|(holder->pitch<<14);
                    direct->bank=bank|(holder->base<<8);
                    direct->palette_surface=const_cast<Image*>(holder);
                } else { direct->palette=slot%palette_slot_count; direct->bank=bank; }
                direct->format=1+(high ? ((d.flags&indexed8_flag) ? 2U : (d.flags&texture_high_nibble_flag) ? 4U : 3U) : (d.flags&indexed8_flag) ? 0U : 1U);
                if (!placeholder) placeholder=image(1,1);
                ++stats.shader_indexed_draws;
                return *placeholder;
            }
            // One image per source view; palette or source changes decode into it again.
            const TextureKey key{{d.texture_base,d.texture_pitch,width,height,
                d.flags&(65|indexed8_flag|texture_high_flag|texture_high_nibble_flag),0xffffffffU}};
            auto found=textures.find(key);
            if (found==textures.end()) {
                auto allocated=image(width,height); texture_bytes+=allocated->allocation_bytes;
                found=textures.emplace(key,Texture{std::move(allocated),r}).first; ++stats.texture_misses;
            }
            auto& tex=found->second; tex.last_use=++texture_use;
            const auto generation=source_revision(r);
            if (!tex.valid || tex.source_revision!=generation || tex.palette_slot!=slot || tex.palette_bank!=bank) {
                PaletteParameters parameters{};
                parameters.mode=1; parameters.slot=slot%palette_slot_count;
                parameters.texture_base=d.texture_base; parameters.texture_pitch=d.texture_pitch;
                parameters.format=high ? ((d.flags&indexed8_flag) ? 2U : (d.flags&texture_high_nibble_flag) ? 4U : 3U) : (d.flags&indexed8_flag) ? 0U : 1U;
                parameters.bank=bank; parameters.width=width; parameters.height=height;
                palette_pass(*tex.image,tex.image.get(),parameters);
                tex.valid=true; tex.source_revision=generation; tex.image->revision=generation;
                tex.palette_slot=slot; tex.palette_bank=bank;
                ++stats.gpu_indexed_decodes;
            }
            return *tex.image;
        }
        if (command.palette_slot) {
            auto copy=command;
            std::array<std::uint32_t,256> words{};
            read_palette_words(command.palette_slot,words);
            snapshot_palette(std::span(copy.palette).first((d.flags&indexed8_flag) ? 256 : 16),words,command.palette_bank);
            copy.palette_slot=0;
            return texture(copy);
        }
    }
    unsigned width=(d.flags&1) ? texture_extent(d.texture_width,d.clamp_u,d.min_u,d.max_u) : 1;
    unsigned height=(d.flags&1) ? texture_extent(d.texture_height,d.clamp_v,d.min_v,d.max_v) : 1;
    if ((d.flags&1) && !(d.flags&64)) {
        const auto found=surfaces.find({d.texture_base,d.texture_pitch,(d.flags&texture_z_flag)!=0});
        if (found!=surfaces.end() && found->second && found->second->valid &&
            texture_fits_surface(d,found->second->width,found->second->height)) {
            width=std::min(width,found->second->width);
            height=std::min(height,found->second->height);
        }
    }
    const TextureKey key{{d.texture_base,d.texture_pitch,width,height,d.flags&(65|indexed8_flag|texture_z_flag|texture_high_flag)},
        (d.flags&64) ? palette_hash(command.palette) : 0};
    const bool high=(d.flags&texture_high_flag)!=0;
    const auto r=(d.flags&1) ? region(d.texture_base,d.texture_pitch,width,height,high ? 0U : (d.flags&indexed8_flag) ? 2U : (d.flags&64) ? 1U : 0U) : Region{};
    auto found_texture=textures.find(key);
    constexpr VkDeviceSize budget=512ULL*1024*1024;
    constexpr std::size_t texture_capacity=4096;
    if (found_texture==textures.end() && !textures.empty() &&
        (textures.size()>=texture_capacity || texture_bytes+VkDeviceSize(width)*height*4>budget)) {
        // A full cache reuses its oldest image of this size. Its descriptors stay
        // valid, and an ordered upload follows any recorded use of the old contents,
        // so nothing drains the GPU. A draw's own levels are the newest lookups.
        auto oldest=textures.end();
        for (auto it=textures.begin();it!=textures.end();++it)
            if (it->second.image->width==width && it->second.image->height==height &&
                it->second.last_use+texture_level_slots<texture_use &&
                (oldest==textures.end() || it->second.last_use<oldest->second.last_use)) oldest=it;
        if (oldest!=textures.end()) {
            auto node=textures.extract(oldest);
            auto recycled=std::move(node.mapped().image); recycled->revision=0;
            node.key()=key; node.mapped()=Texture{std::move(recycled),r};
            found_texture=textures.insert(std::move(node)).position;
            ++stats.texture_recycles; ++stats.texture_misses;
        }
    }
    if (found_texture==textures.end()) {
        auto allocated=image(width,height);
        if (texture_bytes+allocated->allocation_bytes>budget || textures.size()>=texture_capacity) {
            // Retire GPU users and descriptors before releasing old images.
            // Keep recent textures instead of discarding the entire working set.
            clear_views();
            while (!textures.empty() && (texture_bytes+allocated->allocation_bytes>budget*3/4 || textures.size()>=3072)) {
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
    if ((d.flags&64) && tex.palette!=command.palette) { tex.palette=command.palette; tex.valid=false; }
    const auto generation=source_revision(r);
    if (generation!=tex.source_revision) { tex.valid=false; tex.source_revision=generation; }
    // A scaled surface read as a texture keeps every sample.
    if (direct && (d.flags&1) && !(d.flags&(64|texture_z_flag)) && !command.levels.control) {
        if (auto* holder=sampled_surface(d,r)) {
            direct->format=6U; direct->texture_surface=holder; direct->texture_view=holder->base|(holder->pitch<<16);
            if (!placeholder) placeholder=image(1,1);
            ++stats.shader_indexed_draws;
            return *placeholder;
        }
    }
    // Identical surface views use an ordered native image copy for feedback.
    // No CPU conversion is involved in the common framebuffer texture path.
    if ((d.flags&1) && !(d.flags&64)) {
        const bool depth_layout=(d.flags&texture_z_flag)!=0;
        auto found=surfaces.find({d.texture_base,d.texture_pitch,depth_layout});
        if (found!=surfaces.end() && found->second && found->second->valid &&
            found->second->width>=width && found->second->height>=height) {
            // A draw may have resolved only its touched pages. Sampling the
            // image must first resolve all pages of the requested source view.
            auto& from=surface(d.texture_base,d.texture_pitch,height,depth_layout);
            if (!tex.valid || tex.image->revision!=from.revision) {
                order();
                if (from.scale>1) blit_console_samples(from,*tex.image,0,height);
                else {
                    VkImageCopy copy{}; copy.srcSubresource=copy.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.extent={width,height,1};
                    vkCmdCopyImage(this->command,from.image,VK_IMAGE_LAYOUT_GENERAL,tex.image->image,VK_IMAGE_LAYOUT_GENERAL,1,&copy);
                }
                tex.image->revision=from.revision; tex.valid=true; stats.texture_snapshot_bytes+=std::size_t(width)*height*4;
            }
            return *tex.image;
        }
    }
    // Other 32-bit views of rendered memory (offset tiles, other widths) are
    // gathered on the GPU from the memory mirror.
    if ((d.flags&1) && !(d.flags&64)) {
        const bool rendered=std::any_of(surfaces.begin(),surfaces.end(),[&](const auto& entry) {
            return entry.second && entry.second->dirty && entry.second->region.overlaps(r);
        });
        if (rendered && gpu_source(r)) {
            if (direct) {
                direct->format=(d.flags&texture_z_flag) ? 7U : 6U;
                if (!placeholder) placeholder=image(1,1);
                ++stats.shader_indexed_draws;
                return *placeholder;
            }
            const auto fetched=source_revision(r);
            if (!tex.valid || tex.image->revision!=fetched) {
                PaletteParameters parameters{};
                parameters.mode=1; parameters.texture_base=d.texture_base; parameters.texture_pitch=d.texture_pitch; parameters.format=5;
                parameters.layout=(d.flags&texture_z_flag) ? 2U : 0U;
                parameters.width=width; parameters.height=height;
                palette_pass(*tex.image,tex.image.get(),parameters);
                tex.valid=true; tex.image->revision=fetched; ++stats.gpu_indexed_decodes;
            }
            return *tex.image;
        }
    }
    for (auto& [k,i]:surfaces) if (i && i->dirty && i->region.overlaps(r)) {
        ++stats.texture_readbacks; synchronize_source(r); tex.valid=false; break; }
    if (!tex.valid) {
#ifdef ANYPS2_LLVM_TEXTURES
        if ((d.flags&1) && !(d.flags&(texture_z_flag|texture_high_flag))) {
            decode_texture(*tex.image,command,r); tex.valid=true;
            return *tex.image;
        }
#endif
        std::vector<std::uint32_t> pixels(std::size_t(width)*height);
        { Measure timing(stats.texture_decode_ns,profiling);
        if (d.flags&1) for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x) {
            std::uint32_t value;
            if (high) value=command.palette[texture_high_index(d.flags,source[rgba_word(d.texture_base,d.texture_pitch,x,y)])];
            else if (d.flags&indexed8_flag) { const auto byte=indexed_byte(d.texture_base,d.texture_pitch,x,y); value=command.palette[(source[byte/4]>>((byte%4)*8))&255]; }
            else if (d.flags&64) { const auto nibble=indexed_nibble(d.texture_base,d.texture_pitch,x,y); value=command.palette[(source[nibble/8]>>((nibble%8)*4))&15]; }
            else value=source[rgba_word(d.texture_base,d.texture_pitch,x,y)^((d.flags&texture_z_flag) ? 0x600U : 0U)];
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
NativeRaster::Impl::TextureSet NativeRaster::Impl::textures_for(const SpriteCommand& command,IndexedSampling* direct) {
    TextureSet result{};
    const auto levels=texture_lod_levels(command.levels.control);
    // Resolve levels before level 0 so the level-0 lookup remains most recent.
    // Levels the shader decodes share the palette and format of level 0.
    IndexedSampling sampled{};
    const auto lookup=[&](const SpriteCommand& planned,unsigned level) -> Image& {
        auto& image=texture(planned,direct ? &(sampled={}) : nullptr);
        if (sampled.format) { const auto mask=direct->levels|(1U<<level); *direct=sampled; direct->levels=mask; }
        return image;
    };
    if (direct) *direct={};
    for (unsigned level=1;level<=levels;++level) result[level]=&lookup(texture_level_command(command,level),level);
    result[0]=&lookup(command,0);
    for (unsigned level=levels+1;level<texture_level_slots;++level) result[level]=result[0];
    if (direct && direct->palette_surface) result[texture_level_slots-1]=static_cast<Image*>(direct->palette_surface);
    if (direct && direct->texture_surface) { result[texture_level_slots-2]=static_cast<Image*>(direct->texture_surface); direct->levels|=256U; }
    return result;
}
VkDescriptorSet NativeRaster::Impl::bind_target(Image& color,Image& depth,const TextureSet& textures,const Sprite& draw) {
    Measure timing(stats.target_bind_ns,profiling);
    // Clearing views drains the GPU: keep enough descriptor sets for many frames.
    if (sets.size()>=24576) clear_views();
    auto& fb=framebuffers[{&color,&depth}];
    const unsigned height=std::min(color.height,depth.height);
    if (!fb) {
        const VkImageView views[]{color.view,depth.view}; VkFramebufferCreateInfo f{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; f.renderPass=render_pass; f.attachmentCount=2; f.pAttachments=views; f.width=color.width*color.scale; f.height=height*color.scale; f.layers=1;
        if (ordered_fragments) f.attachmentCount=0;
        checked(vkCreateFramebuffer(device,&f,nullptr,&fb),"Create native framebuffer");
    }
    if (!in_pass || current_framebuffer!=fb) {
        order(); VkRenderPassBeginInfo b{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; b.renderPass=render_pass; b.framebuffer=fb; b.renderArea.extent={color.width*color.scale,height*color.scale};
        vkCmdBeginRenderPass(command,&b,VK_SUBPASS_CONTENTS_INLINE); in_pass=true; ++stats.render_passes; current_framebuffer=fb; current_width=color.width; current_height=height;
        const VkViewport viewport{0,0,float(current_width*color.scale),float(current_height*color.scale),0,1};
        const VkRect2D scissor{{0,0},{current_width*color.scale,current_height*color.scale}};
        vkCmdSetViewport(command,0,1,&viewport); vkCmdSetScissor(command,0,1,&scissor);
    } else if (!ordered_fragments && hazards.overlaps(draw.left,draw.top,draw.right,draw.bottom)) {
        flush_batch();
        hazards.clear();
        ++stats.feedback_barriers;
        VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; b.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; b.dstAccessMask=VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_DEPENDENCY_BY_REGION_BIT,1,&b,0,nullptr,0,nullptr);
    }
    if (!ordered_fragments) hazards.add(draw.left,draw.top,draw.right,draw.bottom);
    std::array<Image*,2+texture_level_slots> key{&color,&depth};
    std::copy(textures.begin(),textures.end(),key.begin()+2);
    auto& set=sets[key];
    if (!set) {
        set=allocate_set(descriptors);
        std::array<VkDescriptorImageInfo,texture_level_slots+2> images{};
        for (unsigned level=0;level<texture_level_slots;++level) images[level]={sampler,textures[level]->view,VK_IMAGE_LAYOUT_GENERAL};
        images[texture_level_slots]={VK_NULL_HANDLE,color.view,VK_IMAGE_LAYOUT_GENERAL};
        images[texture_level_slots+1]={VK_NULL_HANDLE,depth.view,VK_IMAGE_LAYOUT_GENERAL};
        std::array<VkWriteDescriptorSet,7> writes{};
        for (unsigned i=0;i<3;++i) {
            writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet=set; writes[i].dstBinding=i;
            writes[i].descriptorCount=i ? 1 : texture_level_slots;
            writes[i].descriptorType=i ? VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo=i ? &images[texture_level_slots+i-1] : images.data();
        }
        if (ordered_fragments) writes[1].descriptorType=writes[2].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        const std::array<VkDescriptorBufferInfo,4> buffers{{{records,0,record_bytes},{mirror,0,VK_WHOLE_SIZE},{palettes,0,VK_WHOLE_SIZE},
            {host_palettes,0,VK_WHOLE_SIZE}}};
        for (unsigned i=0;i<4;++i) {
            auto& w=writes[3+i]; w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet=set; w.dstBinding=3+i; w.descriptorCount=1;
            w.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo=&buffers[i];
        }
        vkUpdateDescriptorSets(device,7,writes.data(),0,nullptr);
    }
    return set;
}
NativeRaster::NativeRaster(VkPhysicalDevice p,VkDevice d,VkQueue q,unsigned family,bool ordered,unsigned resolution_scale):impl_(std::make_unique<Impl>(p,d,q,ordered)) {
    impl_->resolution_scale=resolution_scale;
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
    auto& r=*impl_; r.clear_views(); r.surface_palettes.clear(); r.surface_palette_blocks.reset(); r.surfaces.clear(); r.textures.clear(); r.texture_bytes=0; r.source_revisions={}; r.backing_revisions={}; std::fill(r.exact_words.begin(),r.exact_words.end(),~0ULL); r.mirror_revisions.fill(~0ULL); std::copy(words.begin(),words.end(),r.source.begin()); r.initialized=true; ++r.revision;
    r.field_count=0; r.composed_count=0; r.previous_field=DisplayField::progressive; r.resolved_revision=0;
}
void NativeRaster::read(std::span<std::uint32_t> words,unsigned start,unsigned length) {
    if (words.size()!=store_words || start>=store_words || !length || length>store_words) throw std::runtime_error("Invalid native resource export");
    auto& r=*impl_; Measure timing(r.stats.read_ns,r.profiling);
    r.synchronize_source({start,length}); r.flush();
    ++r.stats.memory_reads; r.stats.memory_read_words+=length; r.read_passes=r.stats.render_passes;
    // Callers read only the range they asked to make current.
    const auto count=std::min<std::size_t>(length,r.source.size()-start);
    std::copy_n(r.source.begin()+start,count,words.begin()+start);
    if (count<length) std::copy_n(r.source.begin(),length-count,words.begin());
}
std::uint32_t NativeRaster::merge_palette(const std::array<std::uint32_t,16>& banks,const std::array<std::uint32_t,256>& current) {
    auto& r=*impl_;
    for (const auto slot:banks) if (slot) r.gather_surface_palette(slot);
    const auto id=r.next_palette++;
    const VkDeviceSize target=VkDeviceSize(id%Impl::palette_slot_count)*256*4;
    r.order();
    for (unsigned bank=0;bank<16;++bank) {
        const VkDeviceSize offset=bank*16*4;
        if (banks[bank]) {
            const VkBufferCopy copy{VkDeviceSize(banks[bank]%Impl::palette_slot_count)*256*4+offset,target+offset,16*4};
            vkCmdCopyBuffer(r.command,r.palettes,r.palettes,1,&copy);
        } else vkCmdUpdateBuffer(r.command,r.palettes,target+offset,16*4,current.data()+bank*16);
    }
    r.order(); ++r.stats.gpu_palette_loads;
    return id;
}
std::uint32_t NativeRaster::load_palette(std::span<const unsigned> addresses,unsigned first) {
    auto& r=*impl_;
    if (addresses.empty() || first+addresses.size()>256) return 0;
    unsigned low=store_words,high=0;
    for (const auto address:addresses) {
        if (address>=store_words) throw std::runtime_error("Invalid native palette address");
        low=std::min(low,address); high=std::max(high,address);
    }
    // Words the CPU holds exactly (host-written, not drawn over) are read by
    // the caller without a stall.
    if (std::all_of(addresses.begin(),addresses.end(),[&](unsigned a){return r.exact_word(a);})) return 0;
    // Backing memory current for every word: the caller reads them without a stall too.
    bool rendered=false;
    for (const auto address:addresses) rendered=rendered || r.backing_revisions[address/2048]<r.source_revisions[address/2048];
    if (!rendered) return 0;
    if (r.surface_palettes.size()>=256) r.gather_surface_palettes();
    // Read in place only from one plain 32-bit surface holding the newest copy of
    // every word, addressed as the shader computes: CSM1 from a whole block base.
    const auto csm1=[](unsigned index) { return (index&1)+((index&6)<<1)+((index&8)>>2)+((index&16)<<2)+((index&96)>>1)+(index&128); };
    const unsigned base=addresses[0]-csm1(first);
    bool regular=base%64==0;
    for (unsigned i=0;regular && i<addresses.size();++i) regular=addresses[i]==base+csm1(first+i);
    if (regular) for (auto& [key,image]:r.surfaces) {
        if (!image || !image->dirty || !image->valid || image->scratch || image->format || image->depth || image->base%32 || !image->pitch) continue;
        const bool holds=std::all_of(addresses.begin(),addresses.end(),[&](unsigned a) {
            const auto page=a/2048;
            const auto relative=(page+512-image->base/32)%512;
            return image->pages[page]==r.source_revisions[page] && (relative/image->pitch)*32+32<=image->height;
        });
        if (!holds) continue;
        const auto id=r.next_palette++;
        r.surface_palettes.push_back({id,{addresses.begin(),addresses.end()},first,image.get()});
        for (const auto address:addresses) r.surface_palette_blocks.set(address/64);
        ++r.stats.surface_palette_loads;
        return id;
    }
    bool newer=false;
    if (!r.gpu_source({low,high-low+1},&newer) || !newer) return 0;
    const auto id=r.next_palette++;
    r.gather_palette(id,addresses,first);
    return id;
}
std::vector<std::uint32_t> NativeRaster::read_surface_samples(unsigned base,unsigned pitch,unsigned format,unsigned& width,unsigned& height) {
    auto& r=*impl_;
    const auto found=r.surfaces.find(std::tuple(base,pitch,format));
    if (found==r.surfaces.end() || !found->second) return {};
    auto& image=*found->second;
    width=image.width*image.scale; height=image.height*image.scale;
    const VkDeviceSize bytes=VkDeviceSize(width)*height*4;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; info.size=bytes; info.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buffer{}; checked(vkCreateBuffer(r.device,&info,nullptr,&buffer),"Create sample readback");
    VkMemoryRequirements requirements{}; vkGetBufferMemoryRequirements(r.device,buffer,&requirements);
    const auto memory=r.allocate(requirements,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    checked(vkBindBufferMemory(r.device,buffer,memory,0),"Bind sample readback");
    r.order();
    VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={width,height,1};
    vkCmdCopyImageToBuffer(r.command,image.image,VK_IMAGE_LAYOUT_GENERAL,buffer,1,&copy);
    r.flush(VK_NULL_HANDLE,VK_NULL_HANDLE,true);
    std::vector<std::uint32_t> samples(std::size_t(width)*height);
    void* mapped=nullptr; checked(vkMapMemory(r.device,memory,0,bytes,0,&mapped),"Map sample readback");
    std::memcpy(samples.data(),mapped,bytes);
    vkUnmapMemory(r.device,memory); vkFreeMemory(r.device,memory,nullptr); vkDestroyBuffer(r.device,buffer,nullptr);
    return samples;
}
void NativeRaster::release_palette(std::uint32_t slot) {
    auto& r=*impl_;
    const auto found=std::find_if(r.surface_palettes.begin(),r.surface_palettes.end(),[&](const auto& palette) { return palette.id==slot; });
    if (found==r.surface_palettes.end()) return;
    r.surface_palettes.erase(found);
    r.surface_palette_blocks.reset();
    for (const auto& palette:r.surface_palettes) for (const auto address:palette.addresses) r.surface_palette_blocks.set(address/64);
}
// A scaled surface a draw can read at each sample's own position: plain 32-bit, holding
// the newest copy of every page of the texture, and not the draw's own target.
NativeRaster::Impl::Image* NativeRaster::Impl::sampled_surface(const Sprite& d,Region source) {
    if (resolution_scale==1 || (d.base==d.texture_base && d.pitch==d.texture_pitch)) return nullptr;
    for (auto& [key,image]:surfaces) {
        if (!image || !image->valid || image->scratch || image->format || image->depth || image->base%32 || !image->pitch ||
            !image->region.overlaps(source)) continue;
        bool holds=true;
        for (unsigned word=source.start,left=source.length;holds && left;) {
            const auto page=(word/2048)%512;
            const auto relative=(page+512-image->base/32)%512;
            holds=image->pages[page]==source_revisions[page] && (relative/image->pitch)*32+32<=image->height;
            const unsigned step=std::min(left,2048-word%2048); word=(word+step)%store_words; left-=step;
        }
        if (holds) return image.get();
    }
    return nullptr;
}
NativeRaster::Impl::SurfacePalette* NativeRaster::Impl::surface_palette(std::uint32_t id) {
    const auto found=std::find_if(surface_palettes.begin(),surface_palettes.end(),[&](const auto& palette) { return palette.id==id; });
    return found==surface_palettes.end() ? nullptr : &*found;
}
void NativeRaster::Impl::gather_palette(std::uint32_t id,std::span<const unsigned> addresses,unsigned first) {
    if (!placeholder) placeholder=image(1,1);
    const auto target=id%palette_slot_count;
    std::array<std::uint32_t,256> list{};
    std::copy(addresses.begin(),addresses.end(),list.begin());
    order();
    vkCmdUpdateBuffer(command,palettes,VkDeviceSize(palette_slot_count+target)*256*4,sizeof(list),list.data());
    PaletteParameters parameters{};
    parameters.mode=0; parameters.slot=target; parameters.addresses=(palette_slot_count+target)*256;
    parameters.count=unsigned(addresses.size()); parameters.bank=first;
    palette_pass(*placeholder,nullptr,parameters);
    ++stats.gpu_palette_loads;
}
void NativeRaster::Impl::gather_surface_palette(std::uint32_t id) {
    const auto* palette=surface_palette(id);
    if (!palette) return;
    const auto addresses=palette->addresses;
    const auto first=palette->first;
    unsigned low=store_words,high=0;
    for (const auto address:addresses) { low=std::min(low,address); high=std::max(high,address); }
    if (!gpu_source({low,high-low+1})) throw std::runtime_error("A surface palette lost its holder");
    gather_palette(id,addresses,first);
    surface_palettes.erase(std::find_if(surface_palettes.begin(),surface_palettes.end(),[&](const auto& entry) { return entry.id==id; }));
    surface_palette_blocks.reset();
    for (const auto& entry:surface_palettes) for (const auto address:entry.addresses) surface_palette_blocks.set(address/64);
}
void NativeRaster::Impl::gather_surface_palettes() {
    while (!surface_palettes.empty()) gather_surface_palette(surface_palettes.front().id);
}
void NativeRaster::Impl::prepare_write(const Sprite& d,unsigned format) {
    if (d.left>=d.right || d.top>=d.bottom) return;
    bool overlaps=d.base%32!=0;
    const unsigned block_width=format ? 16 : 8;
    for (unsigned y=d.top&~7U;!overlaps && y<d.bottom;y+=8)
        for (unsigned x=d.left&~(block_width-1);!overlaps && x<d.right;x+=block_width) {
            const auto word=format ? rgba_halfword(d.base,d.pitch,x,y,format==10)/2 :
                rgba_word(d.base,d.pitch,x,y)^((d.flags&color_z_flag) ? 0x600U : 0U);
            overlaps=surface_palette_blocks[word/64];
        }
    if (!overlaps && (d.flags&depth_test_flag) && (d.flags&depth_write_flag))
        for (unsigned y=d.top&~7U;!overlaps && y<d.bottom;y+=8)
            for (unsigned x=d.left&~7U;!overlaps && x<d.right;x+=8)
                overlaps=surface_palette_blocks[(rgba_word(d.depth_base,d.pitch,x,y)^0x600U)/64];
    if (overlaps) gather_surface_palettes();
}
void NativeRaster::Impl::prepare_write(Region range) {
    for (const auto& palette:surface_palettes)
        for (const auto address:palette.addresses)
            if (range.contains(address)) { gather_surface_palettes(); return; }
}
void NativeRaster::Impl::read_palette_words(std::uint32_t id,std::array<std::uint32_t,256>& words) {
    gather_surface_palette(id);
    order();
    const VkBufferCopy copy{VkDeviceSize(id%palette_slot_count)*256*4,0,256*4};
    vkCmdCopyBuffer(command,palettes,readback,1,&copy); flush();
    std::memcpy(words.data(),readback_mapping,sizeof(words));
    ++stats.palette_readbacks;
}
void NativeRaster::read_palette(std::uint32_t slot,std::span<std::uint32_t,256> words) {
    std::array<std::uint32_t,256> values{};
    impl_->read_palette_words(slot,values);
    std::copy(values.begin(),values.end(),words.begin());
}
void NativeRaster::read_words(std::span<const unsigned> addresses,std::span<std::uint32_t> values) {
    if (addresses.size()!=values.size()) throw std::runtime_error("Invalid native palette read");
    for (auto address:addresses) if (address>=store_words) throw std::runtime_error("Invalid native palette address");
    auto& r=*impl_; for (unsigned i=0;i<addresses.size();++i) {
        if (r.exact_word(addresses[i])) { values[i]=r.source[addresses[i]]; continue; }
        for (const auto& [key,surface]:r.surfaces) if (surface && surface->dirty && surface->region.contains(addresses[i])) {
            ++r.stats.palette_readbacks; break;
        }
        r.synchronize_source({addresses[i],1}); values[i]=r.source[addresses[i]];
    }
}
void NativeRaster::write(std::span<const PixelWrite> writes) {
    for (auto w:writes) if (w.address>=store_words || (w.value&~w.mask)) throw std::runtime_error("Invalid native resource write");
    auto& r=*impl_; Measure timing(r.stats.write_ns,r.profiling);
    if (writes.empty()) return;
    unsigned low=store_words,high=0;
    for (auto w:writes) { low=std::min(low,w.address); high=std::max(high,w.address); }
    const auto targets=r.write_targets({low,high-low+1});
    unsigned first=store_words,last=0;
    for (auto w:writes) {
        if (w.mask==0xffffffffU) r.exact_words[w.address/64]|=1ULL<<(w.address%64);
        auto& value=r.source[w.address]; const auto next=(value&~w.mask)|w.value; if (value==next) continue;
        value=next; first=std::min(first,w.address); last=std::max(last,w.address);
    }
    if (!targets.empty()) {
        r.scatter(writes,targets);
        // A rendered surface can differ from memory even where a write leaves
        // memory unchanged, so every written word counts as a change.
        first=std::min(first,low); last=std::max(last,high);
    }
    r.commit_write(first,last,targets);
}
void NativeRaster::set_dither(std::uint64_t matrix) { impl_->dither_matrix=matrix; }
void NativeRaster::write(const ImageRect& rect) {
    auto& r=*impl_;
    if (!rect.count) return;
    // A rectangle reaching past 2048 pixels wraps and may repeat addresses.
    const auto rows=(rect.first+rect.count-1)/rect.width+1;
    if (!rect.width || rect.x+rect.width>2048 || rect.y+rows>2048) { const auto writes=rect.writes(); write(writes); return; }
    if (rect.bits!=32) { r.write_packed(rect); return; }
    Measure timing(r.stats.write_ns,r.profiling);
    auto& pairs=r.rect_pairs; pairs.resize(rect.words.size()*2);
    unsigned low=store_words,high=0,column=rect.first%rect.width,row=rect.first/rect.width;
    for (std::size_t i=0;i<rect.words.size();++i) {
        const auto address=rgba_word(rect.base,rect.pitch,rect.x+column,rect.y+row);
        pairs[i*2]=address; pairs[i*2+1]=rect.words[i];
        low=std::min(low,address); high=std::max(high,address);
        if (++column==rect.width) { column=0; ++row; }
    }
    const auto targets=r.write_targets({low,high-low+1});
    unsigned first=store_words,last=0;
    for (std::size_t i=0;i<rect.words.size();++i) {
        const auto address=pairs[i*2],value=pairs[i*2+1];
        r.exact_words[address/64]|=1ULL<<(address%64);
        if (r.source[address]==value) continue;
        r.source[address]=value; first=std::min(first,address); last=std::max(last,address);
    }
    if (!targets.empty()) {
        r.scatter_words(pairs,targets);
        // As for write(): rendered surfaces may differ where memory did not.
        first=std::min(first,low); last=std::max(last,high);
    }
    r.commit_write(first,last,targets);
}
std::vector<NativeRaster::Impl::Image*> NativeRaster::Impl::write_targets(Region range) {
    prepare_write(range);
    // Rendered 32-bit views take host writes on the GPU and stay valid, so
    // their newer pages need no readback. Other rendered views publish first.
    std::vector<Image*> targets;
    for (auto& [k,s]:surfaces) if (s && s->dirty && s->region.overlaps(range)) {
        if (s->valid && !s->scratch && (!s->format || s->format==2 || s->format==10)) targets.push_back(s.get());
        else {
            synchronize_source(s->region);
        }
    }
    return targets;
}
void NativeRaster::Impl::commit_write(unsigned first,unsigned last,std::span<Image* const> targets) {
    if (first==store_words) return;
    // Conservative changed range; invalidate each view once per upload,
    // not once per pixel. Sparse/overlapping/masked writes stay ordered.
    const Region changed{first,last-first+1};
    const auto previous=source_revisions;
    ++revision;
    for (auto& [k,s]:surfaces) if (s && s->region.overlaps(changed)) {
        if (std::find(targets.begin(),targets.end(),s.get())==targets.end()) { s->valid=false; continue; }
        // A target current for a page received the writes, so it stays current.
        for (unsigned page=first/2048;page<=last/2048;++page)
            if (s->pages[page]==previous[page]) s->pages[page]=revision;
    }
    touch(changed);
    // Pages a rendered view still holds newer words for stay owned by it;
    // backing memory has only the written words there.
    for (unsigned page=first/2048;page<=last/2048;++page)
        if (previous[page]<=backing_revisions[page]) backing_revisions[page]=revision;
}
void NativeRaster::Impl::scatter_words(std::span<const std::uint32_t> pairs,std::span<Image* const> targets) {
    scatter_entries(pairs,2,targets);
}
void NativeRaster::Impl::scatter(std::span<const PixelWrite> writes,std::span<Image* const> targets) {
    // Merge repeated addresses: the final masked bits equal memory's. A
    // per-word stamp finds repeats in linear time.
    if (scatter_stamps.empty()) { scatter_stamps.assign(store_words,0); scatter_entry.assign(store_words,0); }
    if (++scatter_stamp==0) { std::fill(scatter_stamps.begin(),scatter_stamps.end(),0U); scatter_stamp=1; }
    std::vector<PixelWrite> merged; merged.reserve(writes.size());
    for (const auto w:writes) {
        if (scatter_stamps[w.address]==scatter_stamp) { merged[scatter_entry[w.address]].mask|=w.mask; continue; }
        scatter_stamps[w.address]=scatter_stamp; scatter_entry[w.address]=unsigned(merged.size()); merged.push_back(w);
    }
    std::vector<std::uint32_t> entries(merged.size()*3);
    for (std::size_t i=0;i<merged.size();++i) {
        const auto w=merged[i];
        entries[i*3]=w.address; entries[i*3+1]=source[w.address]&w.mask; entries[i*3+2]=w.mask;
    }
    scatter_entries(entries,3,targets);
}
void NativeRaster::Impl::scatter_entries(std::span<const std::uint32_t> all,unsigned stride,std::span<Image* const> targets) {
    const std::size_t capacity=std::size_t(scatter_capacity)*3/stride,count=all.size()/stride;
    for (std::size_t start=0;start<count;start+=capacity) {
        const auto n=std::min(capacity,count-start);
        const auto entries=all.subspan(start*stride,n*stride);
        const auto bytes=entries.size()*sizeof(std::uint32_t);
        if (bytes>staging_bytes-staging_used) flush();
        prepare_slot();
        const auto offset=slot*staging_bytes+staging_used;
        std::memcpy(static_cast<std::byte*>(mapped)+offset,entries.data(),bytes);
        staging_used+=bytes; order();
        const VkBufferCopy copy{offset,VkDeviceSize(scatter_offset)*4,bytes};
        vkCmdCopyBuffer(command,staging,palettes,1,&copy); stats.upload_bytes+=bytes;
        for (auto* target:targets) {
            PaletteParameters parameters{};
            parameters.mode=stride==2 ? 4 : 2; parameters.surface_base=target->base; parameters.surface_pitch=target->pitch;
            parameters.surface_width=target->width; parameters.surface_height=target->height;
            parameters.addresses=scatter_offset; parameters.count=unsigned(n);
            parameters.owned.fill(~0U);
            parameters.layout=(target->depth ? 1U : 0U)|(target->format ? 4U : 0U)|(target->format==10 ? 8U : 0U);
            palette_pass(*target,nullptr,parameters);
        }
    }
    for (auto* target:targets) target->revision=++revision;
}
void NativeRaster::Impl::write_packed(const ImageRect& rect) {
    // Merge the pixels into whole words once: (address, value, mask) in order.
    if (scatter_stamps.empty()) { scatter_stamps.assign(store_words,0); scatter_entry.assign(store_words,0); }
    if (++scatter_stamp==0) { std::fill(scatter_stamps.begin(),scatter_stamps.end(),0U); scatter_stamp=1; }
    auto& entries=rect_pairs; entries.clear(); entries.reserve(std::size_t(rect.count)*3);
    unsigned low=store_words,high=0;
    for (std::size_t i=0;i<rect.count;++i) {
        unsigned word,shift; std::uint32_t mask; rect.place(i,word,shift,mask);
        const auto value=(rect.value(i)&mask)<<shift; mask<<=shift;
        if (scatter_stamps[word]==scatter_stamp) {
            auto* entry=entries.data()+std::size_t(scatter_entry[word])*3;
            entry[1]=(entry[1]&~mask)|value; entry[2]|=mask; continue;
        }
        scatter_stamps[word]=scatter_stamp; scatter_entry[word]=unsigned(entries.size()/3);
        const auto at=entries.size(); entries.resize(at+3); entries[at]=word; entries[at+1]=value; entries[at+2]=mask;
        low=std::min(low,word); high=std::max(high,word);
    }
    const auto targets=write_targets({low,high-low+1});
    unsigned first=store_words,last=0;
    for (std::size_t i=0;i<entries.size();i+=3) {
        const auto word=entries[i],value=entries[i+1],mask=entries[i+2];
        if (mask==0xffffffffU) exact_words[word/64]|=1ULL<<(word%64);
        const auto next=(source[word]&~mask)|value;
        if (source[word]==next) continue;
        source[word]=next; first=std::min(first,word); last=std::max(last,word);
    }
    if (!targets.empty()) {
        scatter_entries(entries,3,targets);
        first=std::min(first,low); last=std::max(last,high);
    }
    commit_write(first,last,targets);
}
bool NativeRaster::enable_vectors() {
    auto& r=*impl_;
    if (!r.ordered_fragments) return false;
    if (!r.vectors) r.vectors=std::make_unique<NativeVectors>(r.physical,r.device,r.records,record_bytes,frame_slots);
    return true;
}
unsigned NativeRaster::load_vector_kernel(std::span<const std::uint32_t> spirv,unsigned unit) {
    if (!enable_vectors()) throw std::invalid_argument("Vector batches require ordered fragment access for overlapping primitives");
    return impl_->vectors->load_kernel(spirv,unit);
}
void NativeRaster::draw_vectors(const VectorLaunch& launch,const VectorAssembly& assembly,const SpriteCommand& command) {
    auto& r=*impl_; Measure timing(r.stats.draw_ns,r.profiling);
    auto normalized=command.draw;
    normalize_tests(normalized);
    const auto& target=normalized;
    // Textured batches need host-validated ST/Q values, so only host records may sample.
    const auto texture_flags=(launch.kernel==vector_host_records) ? 0U : (1U|perspective_triangle_flag);
    if (!r.initialized || !r.vectors) throw std::invalid_argument("Vector drawing requires initialized storage and enabled vector batches");
    r.vectors->validate(launch);
    const auto count=vector_primitives(assembly.primitive,launch.elements);
    const bool sprites=assembly.primitive==6;
    if (assembly.primitive<3 || assembly.primitive>6 || !count || count>records_per_slot ||
        bool(target.flags&triangle_flag)==sprites || (target.flags&(texture_flags|line_flag)) ||
        (sprites && (target.flags&(shaded_triangle_flag|perspective_triangle_flag|coverage_flag))) ||
        ((target.flags&perspective_triangle_flag) && !(target.flags&1)) ||
        target.right>target.pitch*64 || target.left>=target.right || target.top>=target.bottom ||
        target.left<assembly.scissor[0] || target.right>assembly.scissor[1]+1 ||
        target.top<assembly.scissor[2] || target.bottom>assembly.scissor[3]+1 ||
        assembly.shaded!=unsigned(bool(target.flags&shaded_triangle_flag)) ||
        assembly.coverage!=unsigned(bool(target.flags&coverage_flag)) || assembly.offset_x<0 || assembly.offset_x>65535 ||
        assembly.offset_y<0 || assembly.offset_y>65535 || !(assembly.present&16) || (assembly.present&~31U) ||
        (assembly.xyz_register!=4 && assembly.xyz_register!=5 && assembly.xyz_register!=12 && assembly.xyz_register!=13))
        throw std::invalid_argument("Unsupported vector assembly or target bounds");
    for (const auto& kind:assembly.store) for (const auto store:kind)
        if (store!=vector_constant_word && store>=launch.element_words-1)
            throw std::invalid_argument("Vector register names a missing store word");
    {
        // Target and texture-level rules of the per-draw validator; triangle
        // rules hold by construction or were checked on the host.
        // A fixed positive-area stand-in triangle selects the triangle rules; a
        // stand-in extent does the same for sprites, whose extents the host checked.
        auto shape=target; shape.flags&=~perspective_triangle_flag;
        if (sprites) { shape.dx=1; shape.dy=1; }
        std::array<RasterVertex,3> triangle{};
        if (!sprites) { triangle[1].x=16; triangle[2].y=16; }
        VulkanRenderer::validate(shape,triangle,command.fog_color,command.fog,command.levels);
    }
    if (r.sets.size()>=24576) { r.clear_views(); r.vector_target={}; }
    if (!r.surface_palettes.empty() && target.mask!=0xffffffffU)
        r.prepare_write(target,(target.flags&color_16_flag) ? ((target.flags&color_16s_flag) ? 10 : 2) : 0);
    ++r.surface_scope;
    const unsigned format=(target.flags&color_16_flag) ? ((target.flags&color_16s_flag) ? 10 : 2) : 0;
    // As consecutive CPU draws do, reuse the previous batch's surfaces while
    // they describe the same storage, are tall enough and remain current.
    auto& cached=r.vector_target;
    const auto& previous=cached.draw;
    const bool reuse=cached.color && previous.base==target.base && previous.pitch==target.pitch &&
        (previous.flags&(color_16_flag|color_16s_flag|color_z_flag|depth_test_flag))==(target.flags&(color_16_flag|color_16s_flag|color_z_flag|depth_test_flag)) &&
        (!(target.flags&depth_test_flag) || previous.depth_base==target.depth_base) &&
        cached.color->height>=target.bottom && cached.depth->height>=target.bottom &&
        r.current(*cached.color,target) && r.current(*cached.depth,target);
    auto& color=reuse ? *cached.color : r.surface(target.base,target.pitch,target.bottom,(target.flags&color_z_flag)!=0,false,false,&target,format);
    auto& depth=reuse ? *cached.depth : r.surface((target.flags&depth_test_flag) ? target.depth_base : 0,target.pitch,target.bottom,
        true,!(target.flags&depth_test_flag),false,&target);
    color.scope=depth.scope=r.surface_scope;
    color.last_use=++r.surface_use; depth.last_use=++r.surface_use;
    IndexedSampling indexed{};
    const auto textures=r.textures_for(command,&indexed);
    cached={&color,&depth,target};
    r.begin();
    if (r.records_used+count>records_per_slot || r.host_palettes_used==host_palette_slots ||
        r.vectors->space(r.slot,launch)==NativeVectors::Space::slot_full) {
        r.flush(VK_NULL_HANDLE,VK_NULL_HANDLE,false); r.begin();
    }
    if (r.vectors->space(r.slot,launch)==NativeVectors::Space::resident_full) {
        r.flush(); r.vectors->clear_resident(); r.begin();
    }
    const unsigned first=r.slot*records_per_slot+r.records_used;
    DrawRecord record{target,{},color.width,std::min(color.height,depth.height),command.fog_color,0,
        command.levels.control,command.levels.q,indexed.host ? r.host_palette(*indexed.host) : indexed.palette,indexed.bank,indexed.format,indexed.levels};
    for (unsigned level=0;level<6;++level) record.level_buffers[level]=command.levels.base[level]|(command.levels.pitch[level]<<16);
    if (indexed.texture_surface) record.level_buffers[0]=indexed.texture_view;
    record.dither[0]=std::uint32_t(r.dither_matrix); record.dither[1]=std::uint32_t(r.dither_matrix>>32);
    const auto words=std::bit_cast<std::array<std::uint32_t,NativeVectors::record_words>>(record);
    // The slot's vector command buffer also receives the upload and assembly at submission.
    const auto vector_commands=r.vector_commands();
    r.vectors->record(launch.kernel==vector_host_records ? VK_NULL_HANDLE : vector_commands,r.slot,launch,assembly,words,first,count);
    r.records_used+=count; r.collect_vectors();
    // The GPU determines individual bounds. The host tracks a conservative union
    // (all vertices, or the scissor) for resource ownership and later hazards.
    // Every record carries its own bounds, which the fragment shader enforces.
    // As consecutive CPU draws do, extend the open instanced draw when it uses the
    // same target, pipeline and vertex count and its records are contiguous.
    const auto set=r.bind_target(color,depth,textures,target);
    const auto pipeline=(target.flags&coverage_flag) ? r.coverage_pipeline : r.pipeline;
    const unsigned vertices=sprites ? 6 : 3;
    if (r.batch_count && r.batch_set==set && r.batch_pipeline==pipeline && r.batch_vertices==vertices &&
        r.batch_first+r.batch_count==first) r.batch_count+=count;
    else {
        r.flush_batch();
        r.batch_first=first; r.batch_count=count; r.batch_vertices=vertices; r.batch_set=set; r.batch_pipeline=pipeline;
    }
    ++r.revision;
    if (target.mask!=0xffffffffU) r.written(color,target);
    if (target.flags&depth_write_flag) r.written(depth,target);
    (sprites ? r.stats.sprites : r.stats.triangles)+=count;
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
    Impl::Image *cached_color=nullptr,*cached_depth=nullptr;
    Impl::TextureSet cached_textures{}; IndexedSampling cached_indexed{};
    SpriteCommand cached_command{};
    const auto same_texture=[](const SpriteCommand& a,const SpriteCommand& b) {
        const auto& x=a.draw; const auto& y=b.draw;
        constexpr unsigned formats=65|indexed8_flag|texture_z_flag|texture_high_flag|texture_16_flag|texture_16s_flag;
        return (x.flags&formats)==(y.flags&formats) &&
            x.texture_base==y.texture_base && x.texture_pitch==y.texture_pitch &&
            x.texture_width==y.texture_width && x.texture_height==y.texture_height &&
            x.clamp_u==y.clamp_u && x.clamp_v==y.clamp_v &&
            x.min_u==y.min_u && x.max_u==y.max_u && x.min_v==y.min_v && x.max_v==y.max_v &&
            (!(x.flags&64) || (a.palette==b.palette && a.palette_slot==b.palette_slot && a.palette_bank==b.palette_bank)) &&
            a.levels.control==b.levels.control && a.levels.base==b.levels.base && a.levels.pitch==b.levels.pitch;
    };
    const auto emit=[&](SpriteCommand c,unsigned height) {
        ++r.surface_scope;
        normalize_tests(c.draw);
        const auto& d=c.draw;
        if (r.records_used==records_per_slot || r.host_palettes_used==host_palette_slots) r.flush(VK_NULL_HANDLE,VK_NULL_HANDLE,false);
        else if (r.idle_submit_due()) { ++r.stats.idle_submissions; r.flush(VK_NULL_HANDLE,VK_NULL_HANDLE,false); }
        if (!r.surface_palettes.empty() && d.mask!=0xffffffffU) r.prepare_write(d,(d.flags&color_16_flag) ? ((d.flags&color_16s_flag) ? 10 : 2) : 0);
        const auto& previous=cached_command.draw;
        const bool reuse=cached_color && previous.base==d.base && previous.pitch==d.pitch &&
            (previous.flags&(color_16_flag|color_16s_flag|color_z_flag))==(d.flags&(color_16_flag|color_16s_flag|color_z_flag)) &&
            (previous.flags&depth_test_flag)==(d.flags&depth_test_flag) &&
            (!(d.flags&depth_test_flag) || previous.depth_base==d.depth_base) &&
            cached_color->height>=height && cached_depth->height>=height && same_texture(c,cached_command) &&
            r.current(*cached_color,d) && r.current(*cached_depth,d);
        const unsigned format=(d.flags&color_16_flag) ? ((d.flags&color_16s_flag) ? 10 : 2) : 0;
        auto& color=reuse ? *cached_color : r.surface(d.base,d.pitch,height,(d.flags&color_z_flag)!=0,false,false,&d,format);
        color.scope=r.surface_scope; color.last_use=++r.surface_use;
        auto& depth=reuse ? *cached_depth : r.surface((d.flags&depth_test_flag) ? d.depth_base : 0,d.pitch,height,true,!(d.flags&depth_test_flag),false,&d);
        depth.scope=r.surface_scope; depth.last_use=++r.surface_use;
        IndexedSampling indexed{};
        const auto textures=reuse ? cached_textures : r.textures_for(c,&indexed);
        // The cached draw's stack copy is gone; same_texture compared this palette to it.
        if (reuse) { indexed=cached_indexed; if (indexed.host) indexed.host=&c.palette; }
        if (!reuse) {
            cached_color=nullptr;
            bool disjoint=color.valid && depth.valid && !color.region.overlaps(depth.region);
            for (unsigned level=0;disjoint && level<=texture_lod_levels(c.levels.control);++level) {
                const auto planned=level ? texture_level_command(c,level).draw : d;
                const auto& texture=*textures[level];
                const auto source=(d.flags&1) ? region(planned.texture_base,planned.texture_pitch,
                    ((indexed.levels>>level)&1) ? texture_extent(planned.texture_width,planned.clamp_u,planned.min_u,planned.max_u) : texture.width,
                    ((indexed.levels>>level)&1) ? texture_extent(planned.texture_height,planned.clamp_v,planned.min_v,planned.max_v) : texture.height,
                    (d.flags&texture_high_flag) ? 0U : (d.flags&indexed8_flag) ? 2U : (d.flags&64) ? 1U : 0U) : Region{};
                const bool shader=((indexed.levels>>level)&1)!=0;
                const bool full_texture=!(d.flags&1) || shader ||
                    (texture.width==texture_extent(planned.texture_width,planned.clamp_u,planned.min_u,planned.max_u) &&
                     texture.height==texture_extent(planned.texture_height,planned.clamp_v,planned.min_v,planned.max_v));
                disjoint=full_texture && !source.overlaps(color.region) && !source.overlaps(depth.region);
            }
            if (disjoint) { cached_color=&color; cached_depth=&depth; cached_textures=textures; cached_indexed=indexed; cached_command=c; }
        }
        const auto set=r.bind_target(color,depth,textures,d);
        { Measure timing(r.stats.draw_record_ns,r.profiling); r.append_draw(set,c,indexed); }
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
        field_count=0; field_index=0; composed_count=0; previous_field=DisplayField::progressive; resolved_revision=0;
    }
    Image* inputs[2]{};
    for (unsigned i=0;i<2;++i) if (plan.control&(1U<<i)) {
        const auto& s=plan.surfaces[i];
        inputs[i]=&display_surface(s.base,s.pitch,s.y+(s.height+1)/2,s.format);
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
    auto constants=plan;
    constants.history_valid=(field_count>=3 ? 1U : 0U)|(resolved_revision ? 2U : 0U)|(composed_count>=3 ? 4U : 0U);
    order();
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,field_pipeline);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,field_layout,0,1,&set,0,nullptr);
    vkCmdPushConstants(command,field_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(constants),&constants);
    vkCmdDispatch(command,(plan.width+7)/8,(plan.height+7)/8,1);
    order();
    field_index=(field_index+1)%4; field_count=std::min(field_count+1,3U); composed_count=std::min(composed_count+1,3U);
    previous_field=plan.field;
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
    r.field_count=0; r.composed_count=0; r.previous_field=DisplayField::progressive; r.resolved_revision=0;
    r.synchronize_source({0,store_words});
    return scanout(r.source,plan);
}
bool NativeRaster::changed(const DisplayPlan& plan) {
    auto& r=*impl_; if (!r.initialized) return false;
    std::uint64_t key=1469598103934665603ULL;
    for (unsigned i=0;i<2;++i) if (plan.control&(1U<<i)) {
        const auto& s=plan.surfaces[i]; auto& image=r.display_surface(s.base,s.pitch,s.y+(plan.half_height ? (s.height+1)/2 : s.height),s.format);
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
    r.field_count=0; r.composed_count=0; r.previous_field=DisplayField::progressive; r.resolved_revision=0;
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

std::uint64_t NativeRaster::present(const DisplayPlan& source_plan,VkImage target,VkExtent2D target_extent,VkFormat format,VkSemaphore acquired,VkSemaphore rendered) {
    auto& r=*impl_;
    auto plan=source_plan;
    plan.scale=r.resolution_scale;
    // A scaled surface is already complete detail: show it whole, with no field weave.
    if (plan.scale>1) {
        plan.field=DisplayField::progressive; plan.smoothing=0;
        if (!plan.output_width) { plan.output_width=plan.width; plan.output_height=plan.height; }
    }
    Impl::Image* images[2]{};
    if (plan.field!=DisplayField::progressive) {
        images[0]=images[1]=&r.reconstruct(plan);
        plan.control=2; plan.half_height=0; plan.field=DisplayField::progressive;
        plan.surfaces[1]={0,0,0,0,plan.width,plan.height,0,0};
    } else {
        r.field_count=0; r.composed_count=0; r.previous_field=DisplayField::progressive; r.resolved_revision=0;
        for (unsigned i=0;i<2;++i) if (plan.control&(1U<<i)) { const auto& s=plan.surfaces[i]; images[i]=&r.display_surface(s.base,s.pitch,s.y+(plan.half_height ? (s.height+1)/2 : s.height),s.format); }
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
        VkFramebufferCreateInfo f{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; f.renderPass=r.present_pass; f.attachmentCount=1; f.pAttachments=&view; f.width=target_extent.width; f.height=target_extent.height; f.layers=1;
        checked(vkCreateFramebuffer(r.device,&f,nullptr,&fb),"Create display framebuffer");
    }
    auto& set=r.present_sets[{images[0],images[1]}];
    if (!set) {
        set=r.allocate_set(r.present_descriptors); VkDescriptorImageInfo info[2]{{r.sampler,images[0]->view,VK_IMAGE_LAYOUT_GENERAL},{r.sampler,images[1]->view,VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; w.dstSet=set; w.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.descriptorCount=2; w.pImageInfo=info;
        vkUpdateDescriptorSets(r.device,1,&w,0,nullptr);
    }
    r.order(); VkRenderPassBeginInfo b{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; b.renderPass=r.present_pass; b.framebuffer=fb; b.renderArea.extent=target_extent;
    vkCmdBeginRenderPass(r.command,&b,VK_SUBPASS_CONTENTS_INLINE); r.in_pass=true;
    vkCmdBindPipeline(r.command,VK_PIPELINE_BIND_POINT_GRAPHICS,r.present_pipeline);
    vkCmdBindDescriptorSets(r.command,VK_PIPELINE_BIND_POINT_GRAPHICS,r.present_layout,0,1,&set,0,nullptr);
    // Bars around an image of a different shape than its window.
    const VkClearAttachment bars{VK_IMAGE_ASPECT_COLOR_BIT,0,{.color={.float32={0,0,0,1}}}};
    const VkClearRect whole{{{0,0},target_extent},0,1};
    vkCmdClearAttachments(r.command,1,&bars,1,&whole);
    const VkViewport vp{float(plan.output_x),float(plan.output_y),float(plan.presented_width()),float(plan.presented_height()),0,1};
    const VkRect2D scissor{{std::int32_t(plan.output_x),std::int32_t(plan.output_y)},{plan.presented_width(),plan.presented_height()}};
    vkCmdSetViewport(r.command,0,1,&vp); vkCmdSetScissor(r.command,0,1,&scissor);
    vkCmdPushConstants(r.command,r.present_layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(plan),&plan); vkCmdDraw(r.command,3,1,0,0);
    r.flush(acquired,rendered,false);
    return r.submitted;
}
}
