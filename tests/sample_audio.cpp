#include <nt/ee_audsrv_rpc.hpp>
#include <nt/ee_sif_command_peer.hpp>
#include <nt/ee_bootstrap_modules.hpp>
#include <nt/adpcm_stream.hpp>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
bool protection(std::uint32_t address,std::uint32_t size) { return address<0x1100 && std::uint64_t(address)+size>0x1000; }
class Output final : public nt::audio::Output {
public:
    bool open() override { return available; }
    bool initialize(bool) override { initialized=available; return available; }
    bool configure_voice(unsigned index,const nt::audio::VoiceMix& mix) override { mixes.at(index)=mix; return available; }
    bool configure_core(unsigned,const nt::audio::CoreMix& mix) override { core=mix; return available; }
    bool clear_reverb(unsigned) override { return available; }
    bool submit_stream(unsigned,std::span<const std::int16_t>) override { return false; }
    bool stream_state(unsigned,nt::audio::StreamState&) override { return false; }
    bool stop_stream(unsigned) override { return available; }
    bool stop_voice(unsigned index) override { states.at(index)={}; return available; }
    bool submit_sample(unsigned index,std::span<const std::int16_t> samples,std::uint32_t begin,std::uint32_t length) override {
        if (!available) return false;
        pcm.assign(samples.begin(),samples.end()); loop={begin,length}; states.at(index)={0,true}; return true;
    }
    bool sample_state(unsigned index,nt::audio::StreamState& state) override { state=states.at(index); return available; }
    void close() override { initialized=false; states={}; }
    bool ready() const override { return initialized; }
    unsigned voices() const override { return initialized ? 48 : 0; }
    std::uint32_t error() const override { return available ? 0 : 1; }
    bool available=true,initialized=false;
    std::array<nt::audio::VoiceMix,48> mixes{};
    std::array<nt::audio::StreamState,48> states{};
    nt::audio::CoreMix core;
    std::vector<std::int16_t> pcm;
    std::array<std::uint32_t,2> loop{};
};
}
int main() {
    try {
        using namespace nt::ee;
        std::vector<std::uint8_t> encoded(48);
        auto put=[](auto& bytes,std::size_t offset,std::uint32_t value) { std::memcpy(bytes.data()+offset,&value,4); };
        put(encoded,0,0x4d435041); put(encoded,4,0x10101); put(encoded,8,2048);
        encoded[16]=12; encoded[17]=6; encoded[18]=0x87;
        encoded[32]=12; encoded[33]=3; encoded[34]=0x21;
        nt::audio::Sample decoded;
        check(nt::audio::decode_apcm(encoded,decoded)==nt::audio::SampleError::none,"Decode synthetic looping sample");
        check(decoded.pcm.size()==56 && decoded.pcm[0]==7 && decoded.pcm[1]==-8 && decoded.pcm[28]==1 && decoded.pcm[29]==2,
              "Signed low/high nibble order and block boundaries");
        check(decoded.pitch==2048 && decoded.loop_begin==0 && decoded.loop_length==56,"Loop and pitch metadata");
        auto bad=encoded; bad[16]=0x1c;
        check(nt::audio::decode_apcm(bad,decoded)==nt::audio::SampleError::unsupported && decoded.pcm[0]==7,"History-dependent loops rejected atomically");
        bad=encoded; bad[5]=2;
        check(nt::audio::decode_apcm(bad,decoded)==nt::audio::SampleError::unsupported,"Unsupported stereo format");
        bad=encoded; bad.pop_back();
        check(nt::audio::decode_apcm(bad,decoded)==nt::audio::SampleError::malformed,"Truncated sample");
        bad=encoded; bad[6]=0; bad[17]=0; bad[33]=1; bad[16]=0; bad[18]=0x77; bad[32]=0x40;
        std::fill(bad.begin()+34,bad.end(),0x77);
        check(nt::audio::decode_apcm(bad,decoded)==nt::audio::SampleError::none && decoded.pcm.back()==32767,"Predictor saturation without signed overflow");

        std::array<std::uint8_t,16> raw{}; raw.fill(0x11); raw[0]=0x1c; raw[1]=7;
        nt::audio::RawSample loop;
        check(nt::audio::decode_raw_sample(raw,0,0,loop)==nt::audio::SampleError::none &&
              loop.first_end_frame==28 && loop.sample.loop_begin>0 && loop.sample.loop_length==28,
              "Predictor-dependent loop did not preserve its transient before a stable PCM cycle");
        nt::audio::AdpcmHistory history;
        std::vector<std::int16_t> expanded;
        for (unsigned i=0;i<100;++i) check(nt::audio::decode_adpcm_block(raw,history,expanded)==nt::audio::SampleError::none,"Raw block decode");
        for (unsigned i=0;i<expanded.size();++i) {
            const auto index=i<loop.sample.loop_begin ? i : loop.sample.loop_begin+(i-loop.sample.loop_begin)%loop.sample.loop_length;
            check(expanded[i]==loop.sample.pcm[index],"Native PCM loop differs from continuous predictor history");
        }
        nt::audio::EnvelopeSpec shape{{{{32768,14336,false},{32768,-16384,true},{1,1,false},{32768,-16384,false}}},2048};
        nt::audio::Envelope envelope; envelope.start();
        for (unsigned i=0;i<3;++i) envelope.next(shape);
        check(envelope.state().level==32767 && envelope.state().active,"Native attack did not reach full gain");
        envelope.release(); envelope.next(shape);
        check(envelope.state().active && envelope.state().level==16383,"Release did not preserve the previous gain");
        envelope.next(shape);
        check(!envelope.state().active && !envelope.state().level,"Native release did not reach silence");

        {
            std::array<std::uint8_t,64> bytes{};
            for (unsigned i=0;i<64;i+=16) { bytes[i]=12; bytes[i+2]=0x21; }
            nt::audio::AdpcmStream stream(48,0);
            std::vector<std::int16_t> pcm;
            check(stream.decode(bytes,2,pcm)==nt::audio::SampleError::none && pcm.size()==56 &&
                  stream.pending()[0].address==48 && stream.pending()[1].address==0,
                  "Streaming source wrap lost bounded media data");
            stream.retire(28);
            check(!stream.overlaps(48,16) && stream.overlaps(0,16),"Consumed source blocks retained queue ownership");
            check(stream.source_position(28)==2 && stream.source_position(36)==6,
                  "Compressed source position reported queued rather than consumed PCM");
            bytes[16]=12; bytes[17]=7; bytes[18]=0x43;
            check(stream.decode(bytes,2,pcm)==nt::audio::SampleError::none && pcm[56]==3 && pcm[84]==3 &&
                  stream.pending()[1].end && !stream.finished(),"Updated loop media was not decoded from its current bytes");
            bytes[17]=1;
            check(stream.decode(bytes,8,pcm)==nt::audio::SampleError::none && stream.finished() && pcm.size()==140,
                  "Real stream end did not stop decoding");
        }
        {
            std::array<std::uint8_t,64> bytes;
            bytes.fill(0x21);
            for (unsigned i=0;i<64;i+=16) { bytes[i]=0x1c; bytes[i+1]=0; }
            bytes[1]=4; bytes[49]=3;
            nt::audio::AdpcmStream cursor(0,0);
            std::vector<std::int16_t> original;
            check(cursor.decode(bytes,12,original)==nt::audio::SampleError::none,"Loop revision setup");
            auto replacement=bytes;
            for (unsigned i=0;i<64;i+=16) std::fill_n(replacement.begin()+i+2,14,0x43);
            std::vector<std::int16_t> expected;
            nt::audio::AdpcmHistory history;
            for (unsigned i=0;i<16;++i)
                check(nt::audio::decode_adpcm_block(std::span(i<2 ? bytes : replacement).subspan((i%4)*16,16),history,expected)==nt::audio::SampleError::none,
                      "Independent revised stream decode");
            std::uint64_t begin=0; std::vector<std::int16_t> revised;
            check(cursor.revise(replacement,35,0,64,begin,revised)==nt::audio::SampleError::none && begin==56 &&
                  revised.size()==280 && std::equal(revised.begin(),revised.end(),expected.begin()+56),
                  "Refill lost predictor history, current-block ownership or repeated loop visits");
            std::vector<std::int16_t> tail;
            check(cursor.decode(replacement,4,tail)==nt::audio::SampleError::none &&
                  std::equal(tail.begin(),tail.end(),expected.begin()+336),"Refill did not commit future predictor state");
            const auto saved=cursor;
            replacement[32]=0xfc;
            check(cursor.revise(replacement,35,32,16,begin,revised)==nt::audio::SampleError::unsupported && cursor.frames()==saved.frames(),
                  "Malformed refill changed the committed decoder cursor");
            replacement=bytes; replacement[33]=1;
            // A new end marker ends the sound there; queued buffers keep their length.
            const auto queued=saved.frames();
            check(cursor.revise(replacement,35,32,16,begin,revised)==nt::audio::SampleError::none && cursor.finished() &&
                  revised.size()==queued-begin && revised.back()==0 && cursor.frames()==begin+28,
                  "End marker in queued audio did not end the stream at its block");
        }
        std::vector<std::uint8_t> ram(0x10000);
        Memory memory{}; memory.data=ram.data(); memory.size=ram.size();
        NativeCompletions completions; memory.completions=&completions;
        NativeTransferBuffers buffers; Output output; NativeAudioRpc audio(buffers,output);
        NativeSifReceive receiver; NativeSifLink link(receiver); NativeSifOutbound outbound;
        NativeSifCommandPeer peer(link,memory,protection,0x1000);
        NativeBootstrapModules modules;
        check(peer.register_service(modules) && peer.register_service(buffers) && peer.register_service(audio),"Distinct service endpoints");
        peer.data_upload([&](auto packets) { return buffers.upload(packets); });
        check(peer.connect(outbound),"Connect native upload owner");
        std::array<std::uint8_t,64> args{};
        auto call=[&](NativeRpcService& service,unsigned method,unsigned input,unsigned result=4,unsigned destination=0x3000) {
            return service.call(memory,protection,method,std::span(args).first(input),destination,result);
        };
        put(args,0,encoded.size());
        check(call(buffers,1,4)==Stop::budget,"Allocate owned upload");
        const auto address=nt::le32(ram,0x3000);
        check(address!=0 && buffers.view(address,encoded.size()).size()==encoded.size(),"Allocation is real and bounded");
        std::copy(encoded.begin(),encoded.end(),ram.begin()+0x4000);
        put(ram,0x2000,0x4000); put(ram,0x2004,address); put(ram,0x2008,encoded.size());
        const auto submitted=outbound.submit(memory,0x2000,1);
        check(submitted.status==Stop::budget && submitted.value && outbound.pump(memory)==Stop::budget &&
              outbound.status(submitted.value)==0xffffffff,"Upload completion follows owned copy");
        check(std::equal(encoded.begin(),encoded.end(),buffers.view(address,encoded.size()).begin()),"Immutable uploaded data");
        const std::array<NativeSifPacket,2> invalid{{{address,0,std::vector<std::uint8_t>(16,0xff)},{0x80,0,std::vector<std::uint8_t>(16)}}};
        check(buffers.upload(invalid).status==Stop::invalid_state && buffers.view(address,1)[0]==0x41,"Invalid batch leaves earlier buffer untouched");
        output.available=false;
        check(call(audio,0,64,64)==Stop::unsupported_system && !output.initialized,"No fabricated device initialization");
        output.available=true;
        check(call(audio,0,64,64,0x1000)==Stop::code_write && !output.initialized,"Validate results before opening device");
        check(call(audio,0,64,64)==Stop::budget && output.initialized,"Initialize real provider contract");
        check(call(audio,0x16,4)==Stop::budget,"Initialize sample lifecycle");
        put(args,0,address); put(args,4,encoded.size()); put(args,8,0xabc);
        check(call(audio,0x17,12,16)==Stop::budget && nt::le32(ram,0x3004)==2048,"Decode uploaded owned sample");
        check(call(buffers,2,4)==Stop::budget && buffers.view(address,16).empty(),"Free transport storage after decode");
        put(args,0,0xffffffff); put(args,4,0xabc);
        check(call(audio,0x18,8)==Stop::budget && nt::le32(ram,0x3000)==0 && output.states[24].queued,"Play retained sample on native voice");
        check(output.pcm.size()==56 && output.loop[1]==56 && output.mixes[24].rate==0.5f,"Native PCM loop and rate");
        put(args,0,0);
        check(call(audio,0x18,8)==Stop::budget && nt::le32(ram,0x3000)==std::uint32_t(-7),"Busy channel must not be overwritten");
        output.states[24]={1234,true};
        check(call(audio,0x1d,8)==Stop::budget && nt::le32(ram,0x3000)==1,"Playing state comes from engine");
        put(args,4,0xdef);
        check(call(audio,0x1d,8)==Stop::budget && nt::le32(ram,0x3000)==5,"Unknown playing sample is not a wildcard");
        put(args,4,0xabc);
        output.states[24].queued=false;
        check(call(audio,0x1d,8)==Stop::budget && nt::le32(ram,0x3000)==0,"Completed voice becomes idle");
        put(args,0,0xabc); check(call(audio,0x1c,4)==Stop::budget,"Free decoded sample");
        put(args,0,0); put(args,4,0xabc);
        check(call(audio,0x18,8)==Stop::budget && nt::le32(ram,0x3000)==5,"Freed sample is unavailable");
        check(call(audio,4,4)==Stop::unsupported_system,"Unimplemented PCM RPC is explicit");
        check(call(audio,1,4)==Stop::budget && !output.initialized,"Quit releases native resources");
        std::cout<<"Native sample decode, upload ownership/completion, RPC lifecycle and measured voice state passed\n";
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
