#pragma once
#include <nt/audio.hpp>
#include <nt/iop_native_libraries.hpp>
#include <algorithm>
#include <bit>

namespace nt::iop {
// The converter lowers validated LIBSD public entries to this native owner.
// Unimplemented sound operations stop before changing state or reporting success.
class NativeSoundServices final : public NativeServices {
public:
    NativeSoundServices(NativeLibraryServices& libraries,audio::Output& output) : libraries_(libraries),output_(output) {}
    NativeServiceReply invoke(const NativeServiceRequest& request,std::span<std::uint8_t> ram) override {
        if (request.kind!=NativeServiceKind::libsd) return {};
        if (request.selector==0) {
            if (request.interrupt_context) return {NativeServiceStatus::unavailable};
            if (registered_) return {NativeServiceStatus::unavailable};
            const auto declaration=libraries_.declaration_for_entry("libsd",1,request.call_site);
            if (!declaration || !output_.open()) return {NativeServiceStatus::unavailable};
            const auto registered=libraries_.invoke({6,{*declaration},NativeServiceKind::loadcore},ram);
            if (registered.status!=NativeServiceStatus::returned || registered.value) {
                output_.close(); return {NativeServiceStatus::unavailable};
            }
            registered_=true;
            return {NativeServiceStatus::returned};
        }
        if (!registered_) return {NativeServiceStatus::unavailable};
        if (request.selector==2) { output_.close(); initialized_=false; return returned(); }
        if (request.selector==4) {
            if (request.interrupt_context || request.arguments[0]>1 || !output_.initialize(request.arguments[0]!=0))
                return {NativeServiceStatus::unavailable};
            const auto old=cores_;
            cores_={}; voices_={}; handlers_={};
            if (!request.arguments[0] || samples_.empty()) samples_.assign(sample_capacity,0);
            for (unsigned core=0;core<2;++core) {
                auto& c=cores_[core]; c.mix=core ? 0xffc : 0xff0;
                c.external={static_cast<std::uint16_t>(core ? 0x7fff : 0),static_cast<std::uint16_t>(core ? 0x7fff : 0)};
                c.effect_end=core ? 0x1fffff : 0x1dffff;
                if (request.arguments[0] && initialized_) { c.master=old[core].master; c.depth=old[core].depth; c.effect_end=old[core].effect_end; }
                c.effect[0]=core; c.effect_begin=(c.effect_end+1-16)&~1U;
                if (!apply_core(core,c)) return unavailable();
            }
            initialized_=true; return returned();
        }
        if (!initialized_) return unavailable();
        const auto& a=request.arguments;
        switch (request.selector) {
        case 5: case 6: return parameter(static_cast<std::uint16_t>(a[0]),static_cast<std::uint16_t>(a[1]),request.selector==5);
        case 7: case 8: return routing(static_cast<std::uint16_t>(a[0]),a[1],request.selector==7);
        case 9: case 10: return address(static_cast<std::uint16_t>(a[0]),a[1],request.selector==9);
        case 23: case 24: return effect(a[0]&1U,a[1],request.selector==23,ram);
        case 25: return clear_effect(a[0]&1U,a[1],a[2]);
        case 26: case 27: return handler(request,ram);
        default: return unavailable();
        }
    }
private:
    static constexpr std::uint32_t sample_capacity=2*1024*1024;
    struct Voice {
        std::array<std::uint16_t,2> volume{},envelope{};
        std::uint16_t pitch=0x3fff;
        std::uint32_t start=0x5000,loop=0;
    };
    struct Core {
        std::array<std::uint16_t,2> master{},depth{},external{},input{};
        std::array<std::uint32_t,4> route{0xffffff,0xffffff,0xffffff,0xffffff};
        std::uint16_t mix=0;
        std::uint32_t effect_end=0,effect_begin=0;
        std::array<std::uint32_t,5> effect{};
    };
    struct Handler { std::uint32_t address=0,argument=0; };
    static NativeServiceReply returned(std::uint32_t value=0) { return {NativeServiceStatus::returned,value}; }
    static NativeServiceReply unavailable() { return {NativeServiceStatus::unavailable}; }
    static float fixed_gain(std::uint16_t value) {
        const auto signed_value=(value&0x4000) ? static_cast<int>(value&0x7fff)-0x8000 : static_cast<int>(value&0x7fff);
        return static_cast<float>(signed_value)/16384.0f;
    }
    static float linear_gain(std::uint16_t value) { return static_cast<float>(std::bit_cast<std::int16_t>(value))/32768.0f; }
    static audio::StereoGain gated(const std::array<std::uint16_t,2>& value,std::uint16_t mask,unsigned bit) {
        return {(mask&(1U<<(bit+1))) ? linear_gain(value[0]) : 0,(mask&(1U<<bit)) ? linear_gain(value[1]) : 0};
    }
    bool apply_voice(unsigned index,const Voice& v,const Core& c) {
        const auto bit=1U<<(index%24);
        audio::VoiceMix mix;
        mix.direct={(c.route[0]&bit) && (c.mix&0x800) ? fixed_gain(v.volume[0]) : 0,
                    (c.route[2]&bit) && (c.mix&0x400) ? fixed_gain(v.volume[1]) : 0};
        mix.effect={(c.route[1]&bit) && (c.mix&0x200) ? fixed_gain(v.volume[0]) : 0,
                    (c.route[3]&bit) && (c.mix&0x100) ? fixed_gain(v.volume[1]) : 0};
        mix.rate=static_cast<float>(v.pitch)/4096.0f;
        return output_.configure_voice(index,mix);
    }
    bool apply_core(unsigned core,const Core& c) {
        audio::CoreMix mix;
        mix.output={fixed_gain(c.master[0]),fixed_gain(c.master[1])};
        mix.external_direct=gated(c.external,c.mix,2); mix.external_effect=gated(c.external,c.mix,0);
        mix.input_direct=gated(c.input,c.mix,6); mix.input_effect=gated(c.input,c.mix,4);
        if (!output_.configure_core(core,mix)) return false;
        for (unsigned v=core*24;v<(core+1)*24;++v) if (!apply_voice(v,voices_[v],c)) return false;
        return true;
    }
    NativeServiceReply parameter(std::uint16_t entry,std::uint16_t value,bool writing) {
        const unsigned core=entry&1U,kind=entry>>8,voice=(entry&0x3e)>>1;
        if (kind<=7) {
            if ((entry&0xc0) || voice>=24) return unavailable();
            const auto index=core*24+voice; auto v=voices_[index];
            std::uint16_t* field=nullptr;
            if (kind<2) field=&v.volume[kind];
            else if (kind==2) field=&v.pitch;
            else if (kind<5) field=&v.envelope[kind-3];
            else if (!writing) return returned(kind==5 ? 0 : static_cast<std::uint16_t>(v.volume[kind-6]<<1));
            else return unavailable();
            if (!writing) return returned(*field);
            if (kind<2 && (value&0x8000)) return unavailable(); // Dynamic gain sweeps need a timed envelope.
            if (kind==2 && value>0x3fff) return unavailable();
            if (kind==2 && value>0 && value<4) return unavailable(); // Below the native resampler's range.
            *field=value;
            if (!apply_voice(index,v,cores_[core])) return unavailable();
            voices_[index]=v; return returned();
        }
        if ((entry&0xfe)!=(kind==8 ? 0 : 0x80)) return unavailable();
        auto c=cores_[core]; std::uint16_t* field=nullptr;
        if (kind==8) field=&c.mix;
        else if (kind<=10) field=&c.master[kind-9];
        else if (kind<=12) field=&c.depth[kind-11];
        else if (kind<=14) field=&c.external[kind-13];
        else if (kind<=16) field=&c.input[kind-15];
        else if (kind<=18 && !writing) return returned(static_cast<std::uint16_t>(c.master[kind-17]<<1));
        else return unavailable();
        if (!writing) return returned(*field);
        if ((kind==9 || kind==10) && (value&0x8000)) return unavailable();
        if (kind==8 && value>0xfff) return unavailable();
        *field=value;
        if (!apply_core(core,c)) return unavailable();
        cores_[core]=c; return returned();
    }
    NativeServiceReply routing(std::uint16_t entry,std::uint32_t value,bool writing) {
        if (entry&0xfe) return unavailable();
        const unsigned core=entry&1U,kind=entry>>8; value&=0xffffff;
        if (kind==0x13 || kind==0x14) return (!writing || !value) ? returned() : unavailable();
        if (kind<0x18 || kind>0x1b) return unavailable();
        auto c=cores_[core]; auto& field=c.route[kind-0x18];
        if (!writing) return returned(field);
        field=value;
        if (!apply_core(core,c)) return unavailable();
        cores_[core]=c; return returned();
    }
    NativeServiceReply address(std::uint16_t entry,std::uint32_t value,bool writing) {
        const unsigned core=entry&1U,kind=entry>>8,voice=(entry&0x3e)>>1;
        if (entry&0xc0 || kind<0x1c || kind>0x21 || (kind<0x20 && voice) || voice>=24) return unavailable();
        std::uint32_t* field=nullptr;
        if (kind==0x1d) field=&cores_[core].effect_end;
        else if (kind==0x1c) field=&cores_[core].effect_begin;
        else if (kind==0x20) field=&voices_[core*24+voice].start;
        else if (kind==0x21) field=&voices_[core*24+voice].loop;
        else return unavailable();
        if (!writing) return returned(*field);
        if (value>=sample_capacity) return unavailable();
        *field=kind==0x1d ? value|0x1ffffU : value&~7U;
        return returned();
    }
    NativeServiceReply clear_effect(unsigned core,std::uint32_t channel,std::uint32_t mode) {
        // Byte capacities specified by LIBSD's public effect workspace ABI.
        constexpr std::array<std::uint32_t,10> sizes{16,0x26c0,0x1f40,0x4840,0x6fe0,0xade0,0xf6c0,0x18040,0x18040,0x3c00};
        if (mode>=sizes.size() || channel>1) return returned(0xffffffffU);
        if (!mode) return returned();
        const auto end=cores_[core].effect_end+1;
        if (end>samples_.size() || sizes[mode]>end) return unavailable();
        std::fill(samples_.begin()+end-sizes[mode],samples_.begin()+end,0);
        return returned();
    }
    NativeServiceReply effect(unsigned core,std::uint32_t pointer,bool writing,std::span<std::uint8_t> ram) {
        const auto offset=ram_offset(pointer,4,ram.size());
        if (!offset || 20>ram.size()-*offset) return {NativeServiceStatus::address_fault,0,pointer};
        if (!writing) {
            for (unsigned i=0;i<20;i+=4) if (nt_iop_native_code_word(*offset+i)) return {NativeServiceStatus::code_write,0,pointer};
            for (unsigned i=0;i<5;++i) store_native_word(ram,*offset+i*4,cores_[core].effect[i]);
            return returned();
        }
        std::array<std::uint32_t,5> config{};
        for (unsigned i=0;i<5;++i) config[i]=load_native_word(ram,*offset+i*4);
        if ((config[1]&~0x100U)>9) return returned(0xffffffffU);
        if (config[1]&~0x100U) return unavailable(); // No fabricated reverb output.
        config[0]=core; config[3]=0; config[4]=0;
        auto& c=cores_[core]; c.effect=config; c.depth={static_cast<std::uint16_t>(config[2]),static_cast<std::uint16_t>(config[2]>>16)};
        c.effect_begin=(c.effect_end+1-16)&~1U;
        return returned();
    }
    NativeServiceReply handler(const NativeServiceRequest& request,std::span<std::uint8_t> ram) {
        const bool transfer=request.selector==26;
        const auto& a=request.arguments;
        if (transfer && a[0]>1) return unavailable();
        const auto slot=transfer ? a[0] : 2U;
        const auto target=a[transfer ? 1 : 0];
        if (target) {
            const auto offset=ram_offset(target,4,ram.size());
            if (!offset || !nt_iop_native_instruction(*offset)) return {NativeServiceStatus::unmapped_code,0,target};
        }
        const auto previous=handlers_[slot].address;
        handlers_[slot]={target,a[transfer ? 2 : 1]};
        return returned(previous);
    }
    NativeLibraryServices& libraries_;
    audio::Output& output_;
    std::array<Voice,48> voices_{};
    std::array<Core,2> cores_{};
    std::array<Handler,3> handlers_{};
    std::vector<std::uint8_t> samples_;
    bool registered_=false;
    bool initialized_=false;
};
}
