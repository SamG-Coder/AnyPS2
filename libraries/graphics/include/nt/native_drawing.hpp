#pragma once
#include <nt/graphics_packet.hpp>
#include <nt/sprite_plan.hpp>
#include <nt/image_upload.hpp>
#include <nt/image_download.hpp>
#include <nt/sprite_texture.hpp>
#include <nt/triangle_plan.hpp>
#include <nt/line_plan.hpp>
#include <nt/palette_plan.hpp>
#include <bit>
#include <variant>
#include <deque>
#include <chrono>
#include <map>
#include <string>
#include <cmath>
#include <cstring>

namespace nt::graphics {
// Native command planning for a bounded GIF subset. No CPU/VU instruction code
// enters this interface. A whole batch is validated before any GPU work begins.
class NativeDrawing {
public:
    using Packet=graphics::Packet;
    enum class Completion { synchronous, deferred };
    struct Profile { std::uint64_t parse_ns=0,validate_ns=0,submit_ns=0; };
    Profile* profile=nullptr;
    explicit NativeDrawing(bool resident=false,Completion completion=Completion::synchronous):resident_(resident),completion_(completion) {}
    // The renderer must outlive a resident drawing and be exclusively owned by it.
    const std::vector<std::uint32_t>& pixels() const {
        if (dirty_) { renderer_->read(pixels_); dirty_=false; }
        return pixels_;
    }
    std::uint64_t submitted_draws() const { return submitted_draws_; }
    std::uint64_t completed_finishes() const { return completed_finishes_; }
    const std::optional<Sprite>& last_draw() const { return last_draw_; }
    void reset() { state_={}; download_.clear(); download_offset_=0; }
    std::size_t readback_bytes() const { return download_.size()-download_offset_; }
    void readback(std::span<std::uint8_t> output) {
        if (output.empty() || output.size()%16 || output.size()>download_.size()-download_offset_)
            throw std::invalid_argument("Native graphics readback exceeds prepared output");
        std::copy_n(download_.data()+download_offset_,output.size(),output.data());
        download_offset_+=output.size();
        if (download_offset_==download_.size()) { download_.clear(); download_offset_=0; }
    }
    void poll_completions(VulkanRenderer& renderer) {
        if (pending_finishes_.empty()) return;
        const auto completed=renderer.completed_submission();
        while (!pending_finishes_.empty() && pending_finishes_.front()<=completed) {
            pending_finishes_.pop_front(); ++completed_finishes_;
        }
    }
    // Opt-in GPU primitive assembly for kicked PACKED triangle packets whose
    // layout and GS state the GPU path supports (see plan_vectors). Each such tag
    // becomes one batch drawn from its own vertex records; GS state is still
    // updated from the same registers, so it remains exact. Other packets keep
    // the CPU planner. Requires resident storage and renderer vector support.
    bool assemble_triangles_on_gpu(VulkanRenderer& renderer) {
        if (!resident_ || (renderer_ && renderer_!=&renderer) || !renderer.enable_vector_drawing()) return false;
        gpu_packets_=true;
        return true;
    }
    std::uint64_t gpu_packet_batches() const { return gpu_packet_batches_; }
    // Why tags kept the CPU planner while GPU assembly was enabled, by reason.
    const std::map<std::string,std::uint64_t>& gpu_packet_fallbacks() const { return gpu_packet_fallbacks_; }
    // The converter supplies a precompiled element kernel and a PACKED output
    // layout. This entry never reads vertex results back to the CPU.
    void submit_vectors(VulkanRenderer& renderer,const VectorLaunch& launch) {
        if (!resident_) throw std::invalid_argument("Vector drawing requires resident storage");
        if (renderer_ && renderer_!=&renderer) throw std::invalid_argument("Resident drawing renderer changed");
        const auto plan=plan_vectors(state_,launch.vertex,launch.elements,launch.element_words,false);
        if (!renderer_) { renderer.upload(pixels_); renderer_=&renderer; }
        renderer.draw_vectors(launch,plan.assembly,SpriteCommand{plan.target});
        dirty_=true; last_draw_=plan.target;
        submitted_draws_+=vector_primitives(plan.assembly.primitive,launch.elements);
        // No guessed last-vertex values. A later packet must overwrite these
        // latches (and restart a continued primitive) before consuming them.
        state_.unknown|=plan.written;
        if (plan.assembly.present&2) state_.unknown|=unknown_packed_q;
        else { state_.packed_q=0x3f800000; state_.unknown&=~unknown_packed_q; }
        if (plan.assembly.primitive!=3 || launch.elements%3) state_.unknown|=unknown_vertices;
    }
    void submit(VulkanRenderer& renderer,std::span<const Packet> packets) {
        auto marked=profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const auto account=[&](std::uint64_t Profile::* member) {
            if (!profile) return;
            const auto now=std::chrono::steady_clock::now();
            profile->*member+=std::chrono::duration_cast<std::chrono::nanoseconds>(now-marked).count();
            marked=now;
        };
        // Submission is exclusively owned. Keep ordinary batch storage across
        // calls, but release its contents on success and on validation failure.
        struct ReleaseWorkspace { Workspace& workspace; ~ReleaseWorkspace() { workspace.clear(); } } release{workspace_};
        auto candidate=state_;
        candidate.gpu_packets=gpu_packets_ && resident_;
        candidate.fallbacks=&gpu_packet_fallbacks_;
        auto& draws=workspace_.operations;
        std::size_t total=0;
        for (const auto& packet:packets) {
            if (packet.size()>maximum_batch_bytes/sizeof(Quadword)-total)
                throw std::invalid_argument("Native graphics batch exceeds bound");
            total+=packet.size(); parse(candidate,packet,draws);
        }
        account(&Profile::parse_ns);
        if (draws.empty()) { state_=candidate; return; }
        // Validate every draw before recording any part of this submission.
        // Parsing already validated uploads/palette addresses and state changes.
        for (const auto& operation:draws) if (const auto* draw=std::get_if<Draw>(&operation))
            VulkanRenderer::validate(draw->sprite,draw->triangle,draw->fog_color,draw->fog,draw->levels);
        account(&Profile::validate_ns);
        if (resident_) {
            if (renderer_ && renderer_!=&renderer) throw std::invalid_argument("Resident drawing renderer changed");
            if (!renderer_) { renderer.upload(pixels_); renderer_=&renderer; }
            auto& pending=workspace_.commands;
            pending.reserve(std::min<std::size_t>(256,draws.size()));
            const auto flush=[&] {
                if (pending.empty()) return;
                renderer.draw(pending); pending.clear(); dirty_=true;
            };
            for (const auto& operation:draws) {
                if (const auto* draw=std::get_if<Draw>(&operation)) {
                    SpriteCommand command{draw->sprite,{},draw->triangle,draw->fog_color,draw->fog,draw->levels};
                    snapshot_palette(std::span(command.palette).first((draw->sprite.flags&indexed8_flag) ? 256 : 16),candidate.palette,draw->bank,draw->palette16,draw->texa);
                    pending.push_back(command);
                    if (pending.size()==256) flush();
                    continue;
                }
                // Preserve palette, resource-upload and FINISH boundaries.
                flush();
                if (const auto* batch=std::get_if<PacketVectors>(&operation)) {
                    const VectorLaunch launch{vector_host_records,{},{},batch->elements,batch->words,
                        std::span(batch->layout).first(batch->registers),nullptr,batch->records};
                    SpriteCommand command{batch->target,{},{},batch->fog_color,0,batch->levels};
                    if (batch->target.flags&64)
                        snapshot_palette(std::span(command.palette).first((batch->target.flags&indexed8_flag) ? 256 : 16),
                            candidate.palette,batch->bank,batch->palette16,batch->texa);
                    renderer.draw_vectors(launch,batch->assembly,command);
                    dirty_=true;
                } else if (const auto* load=std::get_if<PaletteLoad>(&operation)) {
                    std::array<std::uint32_t,256> words{};
                    const auto selected=std::span(words).first(load->addresses.size());
                    renderer.read_words(load->addresses,selected);
                    apply_palette(candidate.palette,selected,*load);
                } else if (std::holds_alternative<Finish>(operation)) {
                    // Native queue tokens preserve FINISH ordering without blocking
                    // command consumption on every texture switch.
                    if (completion_==Completion::deferred) {
                        pending_finishes_.push_back(renderer.enqueue_finish());
                        if (pending_finishes_.size()>=1024) renderer.synchronize();
                        poll_completions(renderer);
                    } else renderer.synchronize();
                } else if (const auto* download=std::get_if<ImageDownload>(&operation)) {
                    std::vector<std::uint32_t> storage(1024*1024);
                    renderer.read(storage); download_=download->pack(storage); download_offset_=0;
                } else if (std::holds_alternative<CancelDownload>(operation)) {
                    download_.clear(); download_offset_=0;
                } else {
                    renderer.write(std::get<ImageWrites>(operation)); dirty_=true;
                }
            }
            flush();
            state_=candidate;
            for (const auto& operation:draws) if (const auto* draw=std::get_if<Draw>(&operation)) {
                ++submitted_draws_; last_draw_=draw->sprite;
            } else if (const auto* batch=std::get_if<PacketVectors>(&operation)) {
                submitted_draws_+=vector_primitives(batch->assembly.primitive,batch->elements);
                ++gpu_packet_batches_; last_draw_=batch->target;
            } else if (std::holds_alternative<Finish>(operation) && completion_==Completion::synchronous) ++completed_finishes_;
            account(&Profile::submit_ns);
            return;
        }
        auto pixels=pixels_;
        std::vector<SpriteCommand> sprites;
        const auto flush=[&] {
            if (!sprites.empty()) { renderer.sprites(pixels,sprites); sprites.clear(); }
        };
        for (const auto& operation:draws) {
            if (const auto* draw=std::get_if<Draw>(&operation)) {
                SpriteCommand command{draw->sprite,{},draw->triangle,draw->fog_color,draw->fog,draw->levels};
                snapshot_palette(std::span(command.palette).first((draw->sprite.flags&indexed8_flag) ? 256 : 16),candidate.palette,draw->bank,draw->palette16,draw->texa);
                sprites.push_back(command);
            } else {
                flush();
                if (const auto* load=std::get_if<PaletteLoad>(&operation)) load_palette(candidate.palette,pixels,*load);
                else if (std::holds_alternative<Finish>(operation)) renderer.synchronize();
                else if (const auto* download=std::get_if<ImageDownload>(&operation)) {
                    download_=download->pack(pixels); download_offset_=0;
                } else if (std::holds_alternative<CancelDownload>(operation)) {
                    download_.clear(); download_offset_=0;
                } else for (const auto& write:std::get<ImageWrites>(operation))
                    pixels[write.address]=(pixels[write.address]&~write.mask)|write.value;
            }
        }
        flush();
        // Pixel/state publication is atomic even if validation or GPU work fails.
        pixels_.swap(pixels); state_=candidate;
        for (const auto& operation:draws) if (const auto* draw=std::get_if<Draw>(&operation)) {
            ++submitted_draws_; last_draw_=draw->sprite;
        } else if (std::holds_alternative<Finish>(operation)) ++completed_finishes_;
        account(&Profile::submit_ns);
    }
private:
    using ImageWrites=std::vector<PixelWrite>;
    struct Draw { Sprite sprite; unsigned bank; std::array<RasterVertex,3> triangle{}; std::uint32_t fog_color=0,fog=0; bool palette16=false; std::uint64_t texa=0; TextureLevels levels{}; };
    struct Finish {};
    struct CancelDownload {};
    // A kicked PACKED triangle tag drawn by GPU primitive assembly from its records.
    struct PacketVectors {
        VectorAssembly assembly;
        Sprite target;
        std::array<VectorRegister,5> layout{};
        unsigned registers=0,elements=0,words=0;
        std::vector<std::uint32_t> records;
        TextureLevels levels{};
        unsigned bank=0; bool palette16=false; std::uint64_t texa=0; std::uint32_t fog_color=0;
    };
    using Operation=std::variant<Draw,ImageWrites,PaletteLoad,Finish,ImageDownload,CancelDownload,PacketVectors>;
    struct Workspace {
        std::vector<Operation> operations;
        std::vector<SpriteCommand> commands;
        void clear() {
            operations.clear(); commands.clear();
            // A large one-off upload/batch must not pin its planning allocation.
            if (operations.capacity()>4096) std::vector<Operation>().swap(operations);
        }
    } workspace_;
    struct Context;
    // Games commonly jitter XYOFFSET by a fraction of a pixel every frame (for
    // example a three-step vertical cycle) so that an interlaced CRT blends the
    // images into anti-aliased edges. A progressive display shows that as the
    // whole picture trembling. Changes smaller than one pixel on both axes keep
    // drawing at the established offset, as a modern renderer keeps its camera
    // still; a move of a pixel or more takes effect at once. This deliberately
    // differs from the GS by less than one pixel of placement.
    static void set_offset(Context& context,std::uint64_t value);
    // FBW=0 places a frame's pages side by side along x: for rows 0-31 every
    // address equals that of a 2048-pixel-wide (FBW=32) frame, so a frame
    // scissored to those rows draws with that width exactly. A taller scissor
    // would wrap rows every 32 lines on the GS; it keeps FBW=0, which stops
    // with a diagnostic when drawn.
    static void update_frame(Context& context);
public:
    // Largest vertical XYOFFSET jitter held back since the last call, in 1/16
    // pixel. Call from the thread that submits drawing.
    unsigned take_offset_jitter() {
        unsigned jitter=0;
        for (auto& context:state_.contexts) { jitter=std::max(jitter,unsigned(context.jitter_y)); context.jitter_y=0; }
        return jitter;
    }
private:
    struct Context {
        std::uint64_t frame=0,offset=0,scissor=0,test=0,depth=0,texture=0,filter=0,clamp=0,alpha=0;
        // FRAME as written; frame is the value used for drawing (see FBW=0).
        std::uint64_t written_frame=0;
        // XYOFFSET as written; offset is the stabilized value used for drawing.
        std::uint64_t written_offset=0;
        bool offset_written=false;
        std::uint32_t jitter_y=0; // largest vertical change held back, in 1/16 pixel
        std::array<std::uint64_t,2> mipmap_bases{};
        bool force_alpha=false;
    };
    struct State {
        unsigned unknown=0; // RGBAQ, ST, UV, FOG, then queue and PACKED Q below.
        // While set, triangle kicks update GS state without planning a draw: their
        // primitives are drawn by a PacketVectors batch. Never set across tags.
        bool suppress_draws=false;
        bool gpu_packets=false;
        std::map<std::string,std::uint64_t>* fallbacks=nullptr; // diagnostics only
        // While set, every position write appends a resolved vertex record.
        std::vector<std::uint32_t>* vertex_sink=nullptr;
        std::array<Context,2> contexts;
        unsigned primitive=0,attributes=0,color=0,x=0,y=0;
        unsigned scan_mask=0;
        std::uint32_t uv=0,first_uv=0;
        std::uint64_t texa=0;
        std::uint32_t s=0,t=0,q=0x3f800000,packed_q=0x3f800000;
        std::uint32_t fog_color=0,fog=0;
        bool use_primitive=false,first=false;
        std::array<RasterVertex,3> vertices{};
        unsigned vertex_count=0;
        bool color_clamp=false,per_pixel_blend=false;
        std::uint64_t dither_matrix=0;
        bool dither_enabled=false;
        std::uint64_t transfer_buffer=0,transfer_position=0,transfer_extent=0;
        ImageUpload upload;
        std::array<std::uint32_t,256> palette{};
        std::array<unsigned,2> palette_bases{};
        std::uint64_t palette_config=0;
    } state_;
    static constexpr unsigned unknown_vertices=1U<<5,unknown_packed_q=1U<<6;
    bool gpu_packets_=false;
    std::uint64_t gpu_packet_batches_=0;
    std::map<std::string,std::uint64_t> gpu_packet_fallbacks_;
    struct VectorPlan { VectorAssembly assembly; Sprite target; unsigned written=0; TextureLevels levels{}; };
    // Conservative GS page extent of a surface: 32-bit page geometry (64x32
    // pixels per 2048-word page) covers at least the pages of every format.
    struct Pages { unsigned first=0,count=0; };
    static Pages surface_pages(unsigned first_page,unsigned pitch,unsigned height) {
        return {first_page&511U,std::min(512U,std::max(1U,pitch)*((height+31)/32)+1)};
    }
    static bool overlaps(Pages a,Pages b) {
        const auto inside=[](Pages range,unsigned page) { return ((page-range.first)&511U)<range.count; };
        return inside(a,b.first) || inside(b,a.first);
    }
    // Data-independent part of GPU primitive assembly from the current GS state.
    // Throws std::invalid_argument for anything the GPU path does not reproduce.
    static VectorPlan plan_vectors(const State& state,std::span<const VectorRegister> vertex,unsigned elements,unsigned element_words,
        bool textured,bool reglist=false) {
        const auto primitive=state.primitive&7;
        auto attributes=state.use_primitive ? state.primitive : state.attributes;
        const bool sprites=primitive==6;
        // As NativeDrawing::command: sprites have no edge coverage and ignore FIX.
        if (sprites) attributes&=~(128U|1024U);
        if (primitive<3 || primitive>6 || (attributes&~(sprites ? 0x37fU : 0x3ffU)))
            throw std::invalid_argument("Vector drawing supports triangle lists, strips, fans and sprites only");
        if ((attributes&16) && !textured) throw std::invalid_argument("Textured vector drawing is not supported");
        if (sprites && (attributes&16) && !(attributes&256)) throw std::invalid_argument("Perspective sprite textures are unsupported");
        if (state.vertex_count || state.first || (state.unknown&unknown_vertices))
            throw std::invalid_argument("Vector drawing cannot continue an existing primitive; write PRIM first");
        if (elements<(sprites ? 2U : 3U) || vertex.empty() || vertex.size()>5)
            throw std::invalid_argument("Invalid vector vertex layout or element count");
        VectorPlan plan;
        auto& assembly=plan.assembly;
        for (auto& store:assembly.store) store.fill(vector_constant_word);
        assembly.primitive=primitive; assembly.shaded=bool(attributes&8); assembly.coverage=bool(attributes&128);
        assembly.reglist=reglist ? 1U : 0U;
        for (unsigned i=0;i<vertex.size();++i) {
            const auto& reg=vertex[i];
            unsigned kind=0;
            switch (reg.reg) {
            case 1: kind=0; break;
            case 2:
                // PACKED RGBAQ takes Q from a preceding ST; a REGLIST RGBAQ carries Q.
                if ((assembly.present&1) && !reglist) throw std::invalid_argument("Vector ST must precede RGBAQ");
                kind=1; break;
            case 3: kind=2; break;
            case 10: kind=3; break;
            case 4: case 5: case 12: case 13:
                if (i+1!=vertex.size()) throw std::invalid_argument("Vector position must end the vertex record");
                kind=4; assembly.xyz_register=reg.reg; break;
            default: throw std::invalid_argument("Unsupported vector PACKED register: "+std::to_string(reg.reg));
            }
            if (assembly.present&(1U<<kind)) throw std::invalid_argument("Duplicate vector PACKED register");
            assembly.present|=1U<<kind; assembly.store[kind]=reg.store; assembly.constant[kind]=reg.constant;
            for (auto word:reg.store) if (word!=vector_constant_word && (element_words<2 || word>=element_words-1))
                throw std::invalid_argument("Vector register names a missing store word");
        }
        if (!(assembly.present&16)) throw std::invalid_argument("Vector vertex layout lacks a position");
        plan.written=(assembly.present&15)|((assembly.xyz_register&1) ? 0U : 8U);
        if (state.unknown&15U&~plan.written)
            throw std::invalid_argument("Vector drawing requires GS registers whose values are still GPU-owned");
        const auto& context=state.contexts[(attributes>>9)&1];
        assembly.offset_x=int(context.offset&65535); assembly.offset_y=int((context.offset>>32)&65535);
        assembly.scissor={unsigned(context.scissor&2047),unsigned((context.scissor>>16)&2047),
            unsigned((context.scissor>>32)&2047),unsigned((context.scissor>>48)&2047)};
        assembly.state={state.color,state.q,state.s,state.t,state.uv,state.fog};
        auto& target=plan.target;
        target=sprite_target(context.frame);
        target.left=assembly.scissor[0]; target.right=assembly.scissor[1]+1;
        target.top=assembly.scissor[2]; target.bottom=assembly.scissor[3]+1;
        if (!sprites) target.flags|=triangle_flag|(assembly.shaded ? shaded_triangle_flag : 0);
        else assembly.shaded=0;
        configure_draw(state,context,attributes,target);
        if (attributes&16) {
            plan.levels=plan_texture(target,context.texture,context.filter,context.clamp,state.texa,context.mipmap_bases);
            if (!(attributes&256) && !sprites) target.flags|=perspective_triangle_flag;
            // Every primitive samples one snapshot, so texture storage must not be a
            // target of the same batch (render-to-texture feedback keeps the CPU path).
            const auto color=surface_pages(target.base/32,target.pitch,target.bottom);
            const auto depth=surface_pages(target.depth_base/32,target.pitch,target.bottom);
            for (unsigned level=0;level<=texture_lod_levels(plan.levels.control);++level) {
                const auto base=level ? plan.levels.base[level-1] : target.texture_base;
                const auto pitch=level ? plan.levels.pitch[level-1] : target.texture_pitch;
                const auto texture=surface_pages(base/32,std::max(pitch,(target.texture_width>>level)/64),
                    std::max(1U,target.texture_height>>level)+31);
                if (overlaps(texture,color) || ((target.flags&depth_test_flag) && overlaps(texture,depth)))
                    throw std::invalid_argument("Vector texture may alias its render target");
            }
        }
        return plan;
    }
    // VulkanRenderer::validate's perspective ST/Q range rules, applied on the host
    // to every primitive slot of the batch (culled ones included, conservatively).
    // A failing packet keeps the CPU planner, which then reports its diagnostic.
    static bool perspective_valid(const State& state,const PacketVectors& batch) {
        const auto& a=batch.assembly;
        const auto word=[&](unsigned vertex,unsigned kind,unsigned n) {
            const auto store=a.store[kind][n];
            return store==vector_constant_word ? a.constant[kind][n] : batch.records[std::size_t(vertex)*batch.words+1+store];
        };
        const auto stq=[&](unsigned vertex) {
            const auto s=(a.present&2) ? word(vertex,1,0) : state.s,t=(a.present&2) ? word(vertex,1,1) : state.t;
            const auto q=!(a.present&1) ? state.q : a.reglist ? word(vertex,0,1) : (a.present&2) ? word(vertex,1,2) : 0x3f800000U;
            return std::array<float,3>{std::bit_cast<float>(s),std::bit_cast<float>(t),std::bit_cast<float>(q)};
        };
        const auto width=batch.target.texture_width,height=batch.target.texture_height;
        const auto vertex_valid=[&](const std::array<float,3>& v) {
            const auto [s,t,q]=v;
            return std::isfinite(s) && std::isfinite(t) && std::isfinite(q) && std::abs(q)>=1e-20f && std::abs(q)<=1e20f &&
                std::abs(s)<=1e20f && std::abs(t)<=1e20f &&
                std::abs(double(s)/q*width*16)<=0x3fffffff && std::abs(double(t)/q*height*16)<=0x3fffffff;
        };
        const auto primitives=vector_primitives(a.primitive,batch.elements);
        for (unsigned item=0;item<primitives;++item) {
            const unsigned first=a.primitive==3 ? item*3 : a.primitive==4 ? item : 0;
            const unsigned second=a.primitive==3 ? item*3+1 : item+1,third=a.primitive==3 ? item*3+2 : item+2;
            const auto v0=stq(first),v1=stq(second),v2=stq(third);
            if (!vertex_valid(v0) || !vertex_valid(v1) || !vertex_valid(v2) ||
                std::signbit(v1[2])!=std::signbit(v0[2]) || std::signbit(v2[2])!=std::signbit(v0[2])) return false;
        }
        return true;
    }
    // Words per resolved vertex record: status, color, Q, S, T, UV, fog, XY, Z, kick.
    static constexpr unsigned resolved_words=10;
    static std::array<VectorRegister,5> resolved_layout() {
        std::array<VectorRegister,5> layout{};
        const unsigned regs[]{1,2,3,10,5};
        const std::array<std::array<std::uint32_t,4>,5> stores{{{0,1,vector_constant_word,vector_constant_word},
            {2,3,vector_constant_word,vector_constant_word},{4,vector_constant_word,vector_constant_word,vector_constant_word},
            {5,vector_constant_word,vector_constant_word,vector_constant_word},{6,7,8,vector_constant_word}}};
        for (unsigned n=0;n<5;++n) { layout[n].reg=regs[n]; layout[n].store=stores[n]; }
        return layout;
    }
    static bool vertex_register(unsigned reg) {
        return reg==1 || reg==2 || reg==3 || reg==10 || reg==4 || reg==5 || reg==12 || reg==13 || reg==15;
    }
    // Plans a GPU batch for a run of vertex entries from the current GS state,
    // before its vertices are resolved; state entries end the run.
    static std::optional<PacketVectors> plan_packet(const State& state) {
        const auto fallback=[&](const std::string& reason) -> std::optional<PacketVectors> {
            if (state.fallbacks) ++(*state.fallbacks)[reason];
            return std::nullopt;
        };
        PacketVectors batch;
        batch.layout=resolved_layout(); batch.registers=5; batch.words=resolved_words;
        try {
            const auto plan=plan_vectors(state,batch.layout,(state.primitive&7)==6 ? 2 : 3,batch.words,true,true);
            batch.assembly=plan.assembly; batch.target=plan.target; batch.levels=plan.levels;
        } catch (const std::invalid_argument& error) { return fallback(error.what()); }
        batch.assembly.reglist=2;
        if (batch.target.flags&1) {
            const auto& context=state.contexts[((state.use_primitive ? state.primitive : state.attributes)>>9)&1];
            batch.bank=unsigned((context.texture>>56)&(palette_is16(context.texture) ? 31 : 15));
            batch.palette16=palette_is16(context.texture); batch.texa=state.texa;
        }
        batch.fog_color=state.fog_color;
        return batch;
    }
    // Appends a batch to the previous one when both draw lists, sprites or
    // strips with identical state, so state writes that change nothing split no
    // batch. Records stay in submission order; an appended strip starts with a
    // restart, so no triangle spans the join.
    static bool merge_packet(Operation& previous_operation,PacketVectors& batch) {
        auto* previous=std::get_if<PacketVectors>(&previous_operation);
        if (!previous) return false;
        // Resolved records carry every register, so the state constants are unused.
        auto a=previous->assembly,b=batch.assembly;
        a.state={}; b.state={};
        const auto primitive=a.primitive;
        if ((primitive!=3 && primitive!=4 && primitive!=6) ||
            (primitive!=4 && previous->elements%(primitive==3 ? 3 : 2)) ||
            vector_primitives(primitive,previous->elements+batch.elements)>4096U ||
            std::memcmp(&a,&b,sizeof(a)) || previous->levels.control!=batch.levels.control ||
            previous->levels.base!=batch.levels.base || previous->levels.pitch!=batch.levels.pitch ||
            previous->levels.q!=batch.levels.q || previous->bank!=batch.bank || previous->palette16!=batch.palette16 ||
            previous->texa!=batch.texa || previous->fog_color!=batch.fog_color) return false;
        auto bounds=previous->target;
        bounds.left=batch.target.left; bounds.top=batch.target.top; bounds.right=batch.target.right; bounds.bottom=batch.target.bottom;
        if (std::memcmp(&bounds,&batch.target,sizeof(bounds))) return false;
        auto& target=previous->target;
        target.left=std::min(target.left,batch.target.left); target.top=std::min(target.top,batch.target.top);
        target.right=std::max(target.right,batch.target.right); target.bottom=std::max(target.bottom,batch.target.bottom);
        if (primitive==4) batch.records[1+batch.assembly.store[4][2]]|=2U;
        previous->records.insert(previous->records.end(),batch.records.begin(),batch.records.end());
        previous->elements+=batch.elements;
        return true;
    }
    // Completes a batch from its resolved vertices. False keeps the CPU planner.
    static bool finish_packet(const State& state,PacketVectors& batch) {
        const auto fallback=[&](const std::string& reason) {
            if (state.fallbacks) ++(*state.fallbacks)[reason];
            return false;
        };
        batch.elements=unsigned(batch.records.size()/batch.words);
        const auto primitives=vector_primitives(batch.assembly.primitive,batch.elements);
        if (primitives>4096) return fallback("more than 4096 primitives in one tag");
        if ((batch.target.flags&perspective_triangle_flag) && !perspective_valid(state,batch))
            return fallback("perspective ST/Q outside the validated range");
        const auto& a=batch.assembly;
        const auto x=[&](unsigned vertex) { return int(batch.records[std::size_t(vertex)*batch.words+1+a.store[4][0]]&65535U); };
        const auto y=[&](unsigned vertex) { return int(batch.records[std::size_t(vertex)*batch.words+1+a.store[4][0]]>>16); };
        if (a.primitive==6 && (batch.target.flags&1))
            for (unsigned item=0;item+1<batch.elements;item+=2)
                if (x(item)==x(item+1) || y(item)==y(item+1)) return fallback("textured sprite without extent");
        // Bound the batch by all of its vertices, as plan_sprite bounds a primitive:
        // a superset of every primitive drawn, and the surface extent CPU draws use.
        int low_x=65535,low_y=65535,high_x=0,high_y=0;
        for (unsigned vertex=0;vertex<batch.elements;++vertex) {
            low_x=std::min(low_x,x(vertex)); high_x=std::max(high_x,x(vertex));
            low_y=std::min(low_y,y(vertex)); high_y=std::max(high_y,y(vertex));
        }
        const auto ceil_pixel=[](int value) { return value>=0 ? (value+15)/16 : value/16; };
        const int coverage=a.coverage ? 15 : 0;
        const int left=std::max(ceil_pixel(std::max(0,low_x-coverage)-a.offset_x),int(a.scissor[0]));
        const int right=std::min(ceil_pixel(high_x-a.offset_x),int(a.scissor[1])+1);
        const int top=std::max(ceil_pixel(std::max(0,low_y-coverage)-a.offset_y),int(a.scissor[2]));
        const int bottom=std::min(ceil_pixel(high_y-a.offset_y),int(a.scissor[3])+1);
        // Nothing draws: no batch, and the CPU planner would not draw either.
        if (!primitives || left>=right || top>=bottom) { batch.elements=0; return true; }
        batch.target.left=unsigned(left); batch.target.right=unsigned(right);
        batch.target.top=unsigned(top); batch.target.bottom=unsigned(bottom);
        return true;
    }
    std::vector<std::uint8_t> download_;
    std::size_t download_offset_=0;
    bool resident_=false;
    Completion completion_=Completion::synchronous;
    std::deque<std::uint64_t> pending_finishes_;
    VulkanRenderer* renderer_=nullptr;
    mutable bool dirty_=false;
    mutable std::vector<std::uint32_t> pixels_=std::vector<std::uint32_t>(1024*1024);
    std::uint64_t submitted_draws_=0;
    std::uint64_t completed_finishes_=0;
    std::optional<Sprite> last_draw_;
    static std::uint64_t half(const Quadword& q,unsigned index) {
        return std::uint64_t(q[index*2])|(std::uint64_t(q[index*2+1])<<32);
    }
    static void command(State& state,unsigned reg,std::uint64_t value,std::vector<Operation>& draws) {
        switch (reg) {
        case 0: state.primitive=unsigned(value&2047); state.first=false; state.vertex_count=0; state.unknown&=~unknown_vertices; return;
        case 1: state.color=std::uint32_t(value); state.q=std::uint32_t(value>>32); state.unknown&=~1U; return;
        case 2: state.s=std::uint32_t(value); state.t=std::uint32_t(value>>32); state.unknown&=~2U; return;
        case 3: state.uv=std::uint32_t(value)&0x3fff3fffU; state.unknown&=~4U; return;
        case 0xa: state.fog=std::uint32_t(value>>56); state.unknown&=~8U; return;
        case 6: case 7:
            if (const auto load=plan_palette(value,state.palette_config,state.palette_bases)) draws.push_back(*load);
            state.contexts[reg-6].texture=value; return;
        case 0x16: case 0x17: {
            constexpr std::uint64_t fields=(63ULL<<20)|(~0ULL<<37);
            const auto texture=(state.contexts[reg-0x16].texture&~fields)|(value&fields);
            command(state,reg-0x16+6,texture,draws); return;
        }
        case 0x1c: state.palette_config=value; return;
        case 0x22:
            if ((value&3)==1) throw std::invalid_argument("Reserved native scan mask");
            state.scan_mask=unsigned(value&3); return;
        case 8: case 9: state.contexts[reg-8].clamp=value; return;
        case 0x14: case 0x15: state.contexts[reg-0x14].filter=value; return;
        case 0x34: case 0x35: case 0x36: case 0x37:
            state.contexts[reg&1].mipmap_bases[(reg-0x34)/2]=value; return;
        case 0x3b: state.texa=value; return;
        case 0x3d: state.fog_color=std::uint32_t(value)&0xffffffU; return;
        case 0x3f: return; // Coherent native texture storage has no deferred cache contents.
        case 0x1a: state.use_primitive=(value&1)!=0; return;
        case 0x1b: state.attributes=unsigned(value&2040); return;
        case 0x18: case 0x19: set_offset(state.contexts[reg-0x18],value); return;
        case 0x40: case 0x41: state.contexts[reg-0x40].scissor=value; update_frame(state.contexts[reg-0x40]); return;
        case 0x42: case 0x43: state.contexts[reg-0x42].alpha=value; return;
        case 0x44: state.dither_matrix=value&0x7777777777777777ULL; return;
        case 0x45: state.dither_enabled=(value&1)!=0; return;
        case 0x46: state.color_clamp=(value&1)!=0; return;
        case 0x49: state.per_pixel_blend=(value&1)!=0; return;
        case 0x47: case 0x48: state.contexts[reg-0x47].test=value; return;
        case 0x4a: case 0x4b: state.contexts[reg-0x4a].force_alpha=(value&1)!=0; return;
        case 0x4c: case 0x4d: state.contexts[reg-0x4c].written_frame=value; update_frame(state.contexts[reg-0x4c]); return;
        case 0x4e: case 0x4f: state.contexts[reg-0x4e].depth=value; return;
        case 0x50: state.transfer_buffer=value; return;
        case 0x51: state.transfer_position=value; return;
        case 0x52: state.transfer_extent=value; return;
        case 0x53:
            if ((value&3)==1) {
                draws.emplace_back(ImageDownload::begin(state.transfer_buffer,state.transfer_position,state.transfer_extent));
                state.upload={}; return;
            }
            draws.emplace_back(CancelDownload{});
            state.upload=ImageUpload::begin(state.transfer_buffer,state.transfer_position,state.transfer_extent,unsigned(value&3)); return;
        case 0x61: draws.emplace_back(Finish{}); return;
        case 0xf: case 0x7f: return;
        case 4: case 5: case 12: case 13: break;
        default: throw std::invalid_argument("Unsupported native graphics command: "+std::to_string(reg));
        }
        auto attributes=state.use_primitive ? state.primitive : state.attributes;
        const unsigned x=value&65535,y=(value>>16)&65535;
        const auto z=std::uint32_t(value>>32)&((reg==4 || reg==12) ? 0xffffffU : 0xffffffffU);
        if (reg==4 || reg==12) { state.fog=std::uint32_t(value>>56); state.unknown&=~8U; }
        if (state.vertex_sink) {
            if (state.unknown&(15U|unknown_vertices))
                throw std::invalid_argument("A vertex consumed GPU-owned GS state; write its registers and PRIM first");
            const std::uint32_t resolved[]{0,state.color,state.q,state.s,state.t,state.uv,state.fog,x|(y<<16),z,
                (reg==4 || reg==5) ? 1U : 0U};
            state.vertex_sink->insert(state.vertex_sink->end(),std::begin(resolved),std::end(resolved));
        }
        if (state.unknown&(15U|unknown_vertices))
            throw std::invalid_argument("CPU graphics packet consumes GPU-owned GS state; overwrite registers and restart PRIM");
        const auto primitive=state.primitive&7;
        // Coverage antialiasing belongs to lines and triangles only. A caller
        // may retain AA1 while issuing point draws or rectangular clears.
        if (primitive==0 || primitive==6) attributes&=~128U;
        if (primitive<=2) {
            state.vertices[state.vertex_count++]={int(x),int(y),state.color,state.uv&16383,(state.uv>>16)&16383,
                std::bit_cast<float>(state.s),std::bit_cast<float>(state.t),std::bit_cast<float>(state.q),z,state.fog};
            if (primitive && state.vertex_count<2) return;
            const std::array<RasterVertex,2> vertices{state.vertices[0],state.vertices[primitive ? 1 : 0]};
            state.vertex_count=primitive==2 ? 1 : 0;
            if (primitive==2) state.vertices[0]=vertices[1];
            if (reg==12 || reg==13) return;
            if (attributes&~0x3cfU)
                throw std::invalid_argument("Unsupported native point/line attributes: "+std::to_string(attributes));
            const auto& context=state.contexts[(attributes>>9)&1];
            if (auto draw=plan_line_or_point(context.frame,context.offset,context.scissor,vertices,primitive==0,(attributes&8)!=0,(attributes&128)!=0)) {
                configure_draw(state,context,attributes,draw->draw);
                draws.push_back(Draw{draw->draw,0,draw->triangle});
            }
            return;
        }
        if (primitive>=3 && primitive<=5) {
            state.vertices[state.vertex_count++]={int(x),int(y),state.color,state.uv&16383,(state.uv>>16)&16383,
                std::bit_cast<float>(state.s),std::bit_cast<float>(state.t),std::bit_cast<float>(state.q),z,state.fog};
            if (state.vertex_count<3) return;
            const auto vertices=state.vertices;
            state.vertex_count=primitive==3 ? 0 : 2;
            if (primitive==4) state.vertices[0]=state.vertices[1];
            if (primitive!=3) state.vertices[1]=state.vertices[2];
            if (reg==12 || reg==13 || state.suppress_draws) return;
            if (attributes&~0x3ffU) throw std::invalid_argument("Unsupported native triangle attributes: "+std::to_string(attributes));
            const auto& context=state.contexts[(attributes>>9)&1];
            if (auto draw=plan_triangle(context.frame,context.offset,context.scissor,vertices,(attributes&8)!=0,(attributes&128)!=0)) {
                configure_draw(state,context,attributes,draw->draw);
                TextureLevels levels{};
                if (attributes&16) {
                    levels=plan_texture(draw->draw,context.texture,context.filter,context.clamp,state.texa,context.mipmap_bases);
                    if (!(attributes&256)) draw->draw.flags|=perspective_triangle_flag;
                }
                draws.push_back(Draw{draw->draw,unsigned((context.texture>>56)&(palette_is16(context.texture) ? 31 : 15)),draw->triangle,state.fog_color,state.fog,palette_is16(context.texture),state.texa,levels});
            }
            return;
        }
        // Sprites have no edge coverage interpolation; FIX does not alter their
        // fragment values (the SDK uses it for rectangular clears).
        if (primitive==6) attributes&=~1024U;
        if ((state.primitive&7)!=6 || (attributes&~0x37fU)) {
            if (reg==12 || reg==13) {
                // Vertex writes with a suppressed kick initialize the latch;
                // unsupported primitives still stop when a draw is requested.
                state.x=x; state.y=y; state.first_uv=state.uv; state.first=true; return;
            }
            throw std::invalid_argument("Unsupported native primitive/attributes: "+
                std::to_string(state.primitive&7)+"/"+std::to_string(attributes));
        }
        if (!state.first) { state.x=x; state.y=y; state.first_uv=state.uv; state.first=true; return; }
        state.first=false;
        if (reg==12 || reg==13 || state.suppress_draws) return;
        const auto& context=state.contexts[(attributes>>9)&1];
        auto draw=plan_sprite(context.frame,context.offset,context.scissor,state.x,state.y,x,y,state.color);
        if (draw) {
            draw->depth=z;
            configure_draw(state,context,attributes,*draw);
            TextureLevels levels{};
            if (attributes&16) {
                // Sprites use the Q latched with their final vertex for LOD.
                levels=plan_sprite_texture(*draw,attributes,context.texture,context.filter,context.clamp,state.texa,
                    context.mipmap_bases,context.offset,state.x,state.y,x,y,state.first_uv,state.uv,std::bit_cast<float>(state.q));
            }
            draws.push_back(Draw{*draw,unsigned((context.texture>>56)&(palette_is16(context.texture) ? 31 : 15)),{},state.fog_color,state.fog,palette_is16(context.texture),state.texa,levels});
        }
    }
    static void configure_draw(const State& state,const Context& context,unsigned attributes,Sprite& draw) {
        if (state.dither_enabled && ((context.frame>>24)&63)>1 && (draw.mask&0xffffffU)!=0xffffffU)
            throw std::invalid_argument("Unsupported native dithered surface format");
        if (context.test&0x10000U) {
            const auto format=unsigned((context.depth>>24)&15);
            const auto method=unsigned((context.test>>17)&3);
            const bool write=!(context.depth&(1ULL<<32));
            if ((write || method>=2) && format>1)
                throw std::invalid_argument("Unsupported native depth format");
            draw.depth_base=unsigned(context.depth&511)*32;
            draw.flags|=depth_test_flag|(write ? depth_write_flag : 0)|(format==1 ? depth_24_flag : 0);
        }
        if (state.scan_mask&2) draw.flags|=4096|((state.scan_mask&1)*8192);
        draw.test=std::uint32_t(context.test)&0x7ffffU;
        if (((context.frame>>24)&63)==1) draw.flags|=512;
        if (attributes&128) draw.flags|=coverage_flag;
        if (attributes&64) draw.flags|=alpha_blend_flag;
        if (attributes&32) draw.flags|=fog_flag;
        if (attributes&(64|128)) {
            for (unsigned shift=0;shift<8;shift+=2)
                if (((context.alpha>>shift)&3)>2) throw std::invalid_argument("Unsupported native blend selector");
            draw.blend=std::uint32_t(context.alpha&255)|std::uint32_t((context.alpha>>24)&0xff00);
            draw.flags|=256;
            if (state.per_pixel_blend) draw.flags|=1024;
            if (!state.color_clamp) draw.flags|=2048;
        }
        if (context.force_alpha) draw.flags|=32;
    }
    static void parse(State& state,std::span<const Quadword> packet,std::vector<Operation>& draws) {
        std::size_t cursor=0;
        while (cursor<packet.size()) {
            const auto tag=packet[cursor++]; const auto low=half(tag,0),registers=half(tag,1);
            const auto format=(low>>58)&3,encoded=low>>60,nreg=encoded ? encoded : 16;
            const auto entries=(low&0x7fffU)*nreg;
            if (entries) { state.packed_q=0x3f800000; state.unknown&=~unknown_packed_q; }
            const auto count=format>=2 ? low&0x7fffU : format ? (entries+1)/2 : entries;
            if (count>packet.size()-cursor) throw std::invalid_argument("Truncated native graphics packet");
            if (!format && (low&(1ULL<<46))) command(state,0,(low>>47)&2047,draws);
            if (format>=2) {
                ImageWrites writes;
                if (state.upload.bits) writes.reserve(std::min<std::size_t>(
                    state.upload.width*state.upload.height-state.upload.pixel,
                    (count*128+state.upload.pending_bits)/state.upload.bits));
                state.upload.append(packet.subspan(cursor,count),writes);
                draws.emplace_back(std::move(writes));
            }
            const auto entry_register=[&](std::uint64_t index) {
                const auto reg=unsigned((registers>>((index%nreg)*4))&15);
                return !format && reg==14 ? (packet[cursor+index][2]&255) : reg;
            };
            const auto apply=[&](std::uint64_t index) {
                const auto reg=unsigned((registers>>((index%nreg)*4))&15);
                const auto& q=packet[cursor+(format ? index/2 : index)];
                if (format) {
                    if (reg==14) throw std::invalid_argument("Address/data is invalid in a register-list packet");
                    command(state,reg,half(q,unsigned(index&1)),draws);
                } else if (reg==14) command(state,q[2]&255,half(q,0),draws);
                else if (reg==15) return;
                else if (reg==0) command(state,0,q[0]&2047,draws);
                else if (reg==1) command(state,1,(q[0]&255)|((q[1]&255)<<8)|((q[2]&255)<<16)|((q[3]&255)<<24)|
                    (std::uint64_t(state.packed_q)<<32),draws);
                else if (reg==2) { state.packed_q=q[2]; state.unknown&=~unknown_packed_q; command(state,2,half(q,0),draws); }
                else if (reg==3) command(state,3,(q[0]&16383)|((q[1]&16383)<<16),draws);
                else if (reg==0xa) command(state,0xa,std::uint64_t((q[3]>>4)&255)<<56,draws);
                else if (reg==4 || reg==5 || reg==12 || reg==13) {
                    const auto target=reg>=12 || (q[3]&0x8000U) ? (reg&1)+12U : reg;
                    const auto z=(reg&1) ? q[2] : (q[2]>>4)&0xffffffU;
                    const auto fog=(reg&1) ? 0ULL : (std::uint64_t((q[3]>>4)&255)<<56);
                    command(state,target,(q[0]&65535)|(std::uint64_t(q[1]&65535)<<16)|(std::uint64_t(z)<<32)|fog,draws);
                } else throw std::invalid_argument("Unsupported packed native graphics register");
            };
            // Runs of vertex entries may be drawn by GPU assembly, planned from the
            // GS state at the run; state entries end a run and apply normally, as
            // a modern renderer splits batches at state changes. The GS state
            // machine still applies every register; with draw planning suppressed
            // it resolves each vertex instead. A failed host check replays the run.
            for (std::uint64_t index=0;format<2 && index<entries;) {
                auto end=index;
                bool positions=false;
                while (end<entries && vertex_register(entry_register(end))) {
                    const auto reg=entry_register(end++);
                    positions|=reg==4 || reg==5 || reg==12 || reg==13;
                }
                std::optional<PacketVectors> batch;
                if (state.gpu_packets && positions) batch=plan_packet(state);
                if (batch) {
                    const auto before=state;
                    state.suppress_draws=true; state.vertex_sink=&batch->records;
                    try { for (auto at=index;at<end;++at) apply(at); }
                    catch (...) { state.suppress_draws=false; state.vertex_sink=nullptr; throw; }
                    state.suppress_draws=false; state.vertex_sink=nullptr;
                    if (!finish_packet(state,*batch)) { state=before; for (auto at=index;at<end;++at) apply(at); }
                    else if (batch->elements && !(draws.size() && merge_packet(draws.back(),*batch)))
                        draws.emplace_back(std::move(*batch));
                } else for (auto at=index;at<end;++at) apply(at);
                index=end;
                if (index<entries) apply(index++);
            }
            cursor+=static_cast<std::size_t>(count);
            if (low&0x8000U) {
                if (cursor!=packet.size()) throw std::invalid_argument("Trailing data after graphics end tag");
                return;
            }
        }
        throw std::invalid_argument("Missing native graphics end tag");
    }
};
inline void NativeDrawing::update_frame(Context& context) {
    context.frame=context.written_frame;
    if (!((context.frame>>16)&63) && ((context.scissor>>48)&2047)<32) context.frame|=32ULL<<16;
}
inline void NativeDrawing::set_offset(Context& context,std::uint64_t value) {
    // Components are 12.4 fixed point; 16 units are one pixel.
    const auto distance=[](std::uint64_t a,std::uint64_t b,unsigned shift) {
        const auto x=std::int32_t((a>>shift)&65535U),y=std::int32_t((b>>shift)&65535U);
        return x>y ? x-y : y-x;
    };
    context.written_offset=value;
    if (context.offset_written && distance(value,context.offset,0)<16 && distance(value,context.offset,32)<16) {
        context.jitter_y=std::max(context.jitter_y,std::uint32_t(distance(value,context.offset,32)));
        return;
    }
    context.offset=value; context.offset_written=true;
}
}
