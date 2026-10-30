#pragma once
#include <nt/graphics_packet.hpp>
#include <nt/sprite_plan.hpp>
#include <nt/image_upload.hpp>
#include <nt/sprite_texture.hpp>
#include <variant>

namespace nt::graphics {
// Native command planning for a bounded GIF subset. No CPU/VU instruction code
// enters this interface. A whole batch is validated before any GPU work begins.
class NativeDrawing {
public:
    using Packet=graphics::Packet;
    const std::vector<std::uint32_t>& pixels() const { return pixels_; }
    void submit(VulkanRenderer& renderer,std::span<const Packet> packets) {
        auto candidate=state_;
        std::vector<Operation> draws;
        std::size_t total=0;
        for (const auto& packet:packets) {
            if (packet.size()>65536-total) throw std::invalid_argument("Native graphics batch exceeds bound");
            total+=packet.size(); parse(candidate,packet,draws);
        }
        auto pixels=pixels_;
        for (const auto& operation:draws) {
            if (const auto* draw=std::get_if<Sprite>(&operation)) renderer.sprite(pixels,*draw);
            else for (const auto& write:std::get<ImageWrites>(operation))
                pixels[write.address]=(pixels[write.address]&~write.mask)|write.value;
        }
        // Pixel/state publication is atomic even if validation or GPU work fails.
        pixels_.swap(pixels); state_=candidate;
    }
private:
    using ImageWrites=std::vector<PixelWrite>;
    using Operation=std::variant<Sprite,ImageWrites>;
    struct Context {
        std::uint64_t frame=0,offset=0,scissor=0,test=0,depth=0,texture=0,filter=0,clamp=0;
        bool force_alpha=false;
    };
    struct State {
        std::array<Context,2> contexts;
        unsigned primitive=0,attributes=0,color=0,x=0,y=0;
        std::uint32_t uv=0,first_uv=0,texa=0;
        bool use_primitive=false,first=false;
        std::uint64_t transfer_buffer=0,transfer_position=0,transfer_extent=0;
        ImageUpload upload;
    } state_;
    std::vector<std::uint32_t> pixels_=std::vector<std::uint32_t>(1024*1024);
    static std::uint64_t half(const Quadword& q,unsigned index) {
        return std::uint64_t(q[index*2])|(std::uint64_t(q[index*2+1])<<32);
    }
    static void command(State& state,unsigned reg,std::uint64_t value,std::vector<Operation>& draws) {
        switch (reg) {
        case 0: state.primitive=unsigned(value&2047); state.first=false; return;
        case 1: state.color=std::uint32_t(value); return;
        case 3: state.uv=std::uint32_t(value)&0x3fff3fffU; return;
        case 6: case 7: state.contexts[reg-6].texture=value; return;
        case 8: case 9: state.contexts[reg-8].clamp=value; return;
        case 0x14: case 0x15: state.contexts[reg-0x14].filter=value; return;
        case 0x3b: state.texa=std::uint32_t(value); return;
        case 0x3f: return; // Coherent native texture storage has no deferred cache contents.
        case 0x1a: state.use_primitive=(value&1)!=0; return;
        case 0x1b: state.attributes=unsigned(value&2040); return;
        case 0x18: case 0x19: state.contexts[reg-0x18].offset=value; return;
        case 0x40: case 0x41: state.contexts[reg-0x40].scissor=value; return;
        case 0x47: case 0x48: state.contexts[reg-0x47].test=value; return;
        case 0x4a: case 0x4b: state.contexts[reg-0x4a].force_alpha=(value&1)!=0; return;
        case 0x4c: case 0x4d: state.contexts[reg-0x4c].frame=value; return;
        case 0x4e: case 0x4f: state.contexts[reg-0x4e].depth=value; return;
        case 0x50: state.transfer_buffer=value; return;
        case 0x51: state.transfer_position=value; return;
        case 0x52: state.transfer_extent=value; return;
        case 0x53:
            state.upload=ImageUpload::begin(state.transfer_buffer,state.transfer_position,state.transfer_extent,unsigned(value&3)); return;
        case 0xf: case 0x7f: return;
        case 5: case 13: break;
        default: throw std::invalid_argument("Unsupported native graphics command: "+std::to_string(reg));
        }
        const auto attributes=state.use_primitive ? state.primitive : state.attributes;
        if ((state.primitive&7)!=6 || (attributes&~0x317U))
            throw std::invalid_argument("Unsupported native sprite attributes");
        const unsigned x=value&65535,y=(value>>16)&65535;
        if (!state.first) { state.x=x; state.y=y; state.first_uv=state.uv; state.first=true; return; }
        state.first=false;
        if (reg==13) return;
        const auto& context=state.contexts[(attributes>>9)&1];
        if ((context.test&0x4001U) || ((context.test&0x10000U) &&
            (!(context.depth&(1ULL<<32)) || ((context.test>>17)&3)!=1)))
            throw std::invalid_argument("Unsupported native sprite alpha or depth test");
        auto draw=plan_sprite(context.frame,context.offset,context.scissor,state.x,state.y,x,y,state.color);
        if (draw) {
            if (context.force_alpha) draw->flags|=32;
            if (attributes&16) {
                if (((context.texture>>20)&63)>1) throw std::invalid_argument("Native texture palette/format unsupported");
                plan_sprite_texture(*draw,attributes,context.texture,context.filter,context.clamp,state.texa,context.offset,
                    state.x,state.y,x,y,state.first_uv,state.uv);
            }
            draws.push_back(*draw);
        }
    }
    static void parse(State& state,std::span<const Quadword> packet,std::vector<Operation>& draws) {
        std::size_t cursor=0;
        while (cursor<packet.size()) {
            const auto tag=packet[cursor++]; const auto low=half(tag,0),registers=half(tag,1);
            const auto format=(low>>58)&3,encoded=low>>60,nreg=encoded ? encoded : 16;
            const auto entries=(low&0x7fffU)*nreg;
            const auto count=format>=2 ? low&0x7fffU : format ? (entries+1)/2 : entries;
            if (count>packet.size()-cursor) throw std::invalid_argument("Truncated native graphics packet");
            if (!format && (low&(1ULL<<46))) command(state,0,(low>>47)&2047,draws);
            if (format>=2) {
                ImageWrites writes;
                for (std::size_t index=0;index<count;++index) state.upload.append(packet[cursor+index],writes);
                draws.emplace_back(std::move(writes));
            }
            for (std::uint64_t index=0;format<2 && index<entries;++index) {
                const auto reg=unsigned((registers>>((index%nreg)*4))&15);
                const auto& q=packet[cursor+(format ? index/2 : index)];
                if (format) {
                    if (reg==14) throw std::invalid_argument("Address/data is invalid in a register-list packet");
                    command(state,reg,half(q,unsigned(index&1)),draws);
                } else if (reg==14) command(state,q[2]&255,half(q,0),draws);
                else if (reg==15) continue;
                else if (reg==0) command(state,0,q[0]&2047,draws);
                else if (reg==1) command(state,1,(q[0]&255)|((q[1]&255)<<8)|((q[2]&255)<<16)|((q[3]&255)<<24),draws);
                else if (reg==3) command(state,3,(q[0]&16383)|((q[1]&16383)<<16),draws);
                else if (reg==5 || reg==13) {
                    const auto target=reg==13 || (q[3]&0x8000U) ? 13U : 5U;
                    command(state,target,(q[0]&65535)|(std::uint64_t(q[1]&65535)<<16),draws);
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
