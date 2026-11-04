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
        for (const auto& operation:draws) if (const auto* draw=std::get_if<Draw>(&operation)) {
            const SpriteCommand command{draw->sprite,{},draw->triangle,draw->fog_color,draw->fog,draw->levels};
            VulkanRenderer::validate(std::span(&command,1));
        }
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
                if (const auto* load=std::get_if<PaletteLoad>(&operation)) {
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
    using Operation=std::variant<Draw,ImageWrites,PaletteLoad,Finish,ImageDownload,CancelDownload>;
    struct Workspace {
        std::vector<Operation> operations;
        std::vector<SpriteCommand> commands;
        void clear() {
            operations.clear(); commands.clear();
            // A large one-off upload/batch must not pin its planning allocation.
            if (operations.capacity()>4096) std::vector<Operation>().swap(operations);
        }
    } workspace_;
    struct Context {
        std::uint64_t frame=0,offset=0,scissor=0,test=0,depth=0,texture=0,filter=0,clamp=0,alpha=0;
        std::array<std::uint64_t,2> mipmap_bases{};
        bool force_alpha=false;
    };
    struct State {
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
        case 0: state.primitive=unsigned(value&2047); state.first=false; state.vertex_count=0; return;
        case 1: state.color=std::uint32_t(value); state.q=std::uint32_t(value>>32); return;
        case 2: state.s=std::uint32_t(value); state.t=std::uint32_t(value>>32); return;
        case 3: state.uv=std::uint32_t(value)&0x3fff3fffU; return;
        case 0xa: state.fog=std::uint32_t(value>>56); return;
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
        case 0x18: case 0x19: state.contexts[reg-0x18].offset=value; return;
        case 0x40: case 0x41: state.contexts[reg-0x40].scissor=value; return;
        case 0x42: case 0x43: state.contexts[reg-0x42].alpha=value; return;
        case 0x44: state.dither_matrix=value&0x7777777777777777ULL; return;
        case 0x45: state.dither_enabled=(value&1)!=0; return;
        case 0x46: state.color_clamp=(value&1)!=0; return;
        case 0x49: state.per_pixel_blend=(value&1)!=0; return;
        case 0x47: case 0x48: state.contexts[reg-0x47].test=value; return;
        case 0x4a: case 0x4b: state.contexts[reg-0x4a].force_alpha=(value&1)!=0; return;
        case 0x4c: case 0x4d: state.contexts[reg-0x4c].frame=value; return;
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
        if (reg==4 || reg==12) state.fog=std::uint32_t(value>>56);
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
            if (reg==12 || reg==13) return;
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
        if (reg==12 || reg==13) return;
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
            if (entries) state.packed_q=0x3f800000;
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
            unsigned register_index=0;
            for (std::uint64_t index=0;format<2 && index<entries;++index) {
                const auto reg=unsigned((registers>>(register_index*4))&15);
                if (++register_index==nreg) register_index=0;
                const auto& q=packet[cursor+(format ? index/2 : index)];
                if (format) {
                    if (reg==14) throw std::invalid_argument("Address/data is invalid in a register-list packet");
                    command(state,reg,half(q,unsigned(index&1)),draws);
                } else if (reg==14) command(state,q[2]&255,half(q,0),draws);
                else if (reg==15) continue;
                else if (reg==0) command(state,0,q[0]&2047,draws);
                else if (reg==1) command(state,1,(q[0]&255)|((q[1]&255)<<8)|((q[2]&255)<<16)|((q[3]&255)<<24)|
                    (std::uint64_t(state.packed_q)<<32),draws);
                else if (reg==2) { state.packed_q=q[2]; command(state,2,half(q,0),draws); }
                else if (reg==3) command(state,3,(q[0]&16383)|((q[1]&16383)<<16),draws);
                else if (reg==0xa) command(state,0xa,std::uint64_t((q[3]>>4)&255)<<56,draws);
                else if (reg==4 || reg==5 || reg==12 || reg==13) {
                    const auto target=reg>=12 || (q[3]&0x8000U) ? (reg&1)+12U : reg;
                    const auto z=(reg&1) ? q[2] : (q[2]>>4)&0xffffffU;
                    const auto fog=(reg&1) ? 0ULL : (std::uint64_t((q[3]>>4)&255)<<56);
                    command(state,target,(q[0]&65535)|(std::uint64_t(q[1]&65535)<<16)|(std::uint64_t(z)<<32)|fog,draws);
                } else throw std::invalid_argument("Unsupported packed native graphics register");
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
}
