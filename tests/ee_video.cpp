#include <nt/ee_platform.hpp>
#include <cmath>
#include <cstring>
#include <numbers>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" nt::ee::Stop nt_ee_generated_run(nt::runtime::State*,nt::ee::Memory*,std::uint64_t);
namespace {
void check(bool value,std::source_location location=std::source_location::current()) {
    if (!value) throw std::runtime_error("Native video mismatch at line "+std::to_string(location.line()));
}
using nt::ee::Stop;
struct Fixture {
    std::vector<std::uint8_t> ram=std::vector<std::uint8_t>(4096,0xcd),scratch=std::vector<std::uint8_t>(16384,0xcd);
    nt::ee::NativePlatform platform;
    nt::ee::Memory memory{ram.data(),ram.size()};
    nt::runtime::State state;
    Fixture() {
        platform.attach(memory); memory.scratchpad=scratch.data(); memory.scratchpad_bytes=scratch.size();
        platform.video_output.protection=[](std::uint32_t address,std::uint32_t) { return address<256; };
    }
    Stop write(unsigned address,unsigned value) {
        state.pc=0x3d0020; state.set(4,address); state.set(8,value);
        return nt_ee_generated_run(&state,&memory,1);
    }
    unsigned read(unsigned address) {
        state.pc=0x3d0028; state.set(4,address);
        check(nt_ee_generated_run(&state,&memory,1)==Stop::budget); return static_cast<unsigned>(state.gpr[9].lo);
    }
    void upload(std::span<const std::uint8_t> bytes) {
        check(bytes.size()%16==0);
        for (unsigned i=0;i<bytes.size();i+=16) {
            state.pc=0x3d0000; state.set(4,0x10007010); std::memcpy(&state.gpr[8],bytes.data()+i,16);
            check(nt_ee_generated_run(&state,&memory,1)==Stop::budget);
        }
    }
    void matrix(unsigned index) {
        check(write(0x10002000,0x50000000|(index<<27))==Stop::budget);
        std::array<std::uint8_t,64> data; data.fill(16); upload(data);
        acknowledge();
    }
    void acknowledge() { check(write(0x1000f000,256)==Stop::budget); }
    void destination(unsigned address,unsigned qwords=48) {
        check(write(0x1000b010,address)==Stop::budget && write(0x1000b020,qwords)==Stop::budget);
        check(write(0x1000b000,0x100)==Stop::budget);
    }
    std::array<std::int16_t,384> samples(bool from_scratch=false) {
        std::array<std::int16_t,384> result;
        std::memcpy(result.data(),(from_scratch ? scratch.data() : ram.data())+512,sizeof(result)); return result;
    }
};
std::vector<std::uint8_t> stream(std::string bits) {
    // Original synthetic macroblock followed by a byte-aligned sequence header.
    while (bits.size()%8) bits+='0';
    bits+="00000000000000000000000110110011";
    std::vector<std::uint8_t> result(((bits.size()+127)/128)*16);
    for (unsigned i=0;i<bits.size();++i) if (bits[i]=='1') result[i/8]|=1U<<(7-i%8);
    return result;
}
void check_idct() {
    // Independent floating-point definition, not the production integer basis.
    std::uint32_t seed=17;
    for (unsigned trial=0;trial<24;++trial) {
        std::array<std::int16_t,64> input;
        for (auto& value:input) { seed=seed*1664525U+1013904223U; value=static_cast<std::int16_t>(int(seed&4095)-2048); }
        const auto actual=nt::media::mpeg_idct(input);
        for (unsigned y=0;y<8;++y) for (unsigned x=0;x<8;++x) {
            double sum=0;
            for (unsigned v=0;v<8;++v) for (unsigned u=0;u<8;++u)
                sum+=input[v*8+u]*(u ? 1 : std::sqrt(.5))*(v ? 1 : std::sqrt(.5))*.25*
                    std::cos((2*x+1)*u*std::numbers::pi/16)*std::cos((2*y+1)*v*std::numbers::pi/16);
            check(std::abs(actual[y*8+x]-std::floor(sum+.5))<=1);
        }
    }
}
}
void run_video_block_tests() {
    check_idct();
    for (const bool field:{false,true}) {
        Fixture f; f.matrix(0);
        check(f.write(0x1000f590,0x11201)==Stop::budget);
        f.destination(0x80000200);
        f.upload(stream("10010" "00110" "00110" "00110" "01010" "101010"));
        check(f.write(0x10002000,0x2c080000|(field ? 1U<<25 : 0))==Stop::budget);
        check(f.platform.video.decoded_blocks()==1 && (f.read(0x10002010)&0x80000000)!=0);
        check(f.scratch[512]==0xcd && !(f.platform.completions.status()&8));
        check(f.write(0x1000f590,0x1201)==Stop::budget);
        check(!(f.read(0x1000b000)&256) && f.read(0x1000b010)==0x80000500 && f.read(0x1000b020)==0);
        check((f.platform.completions.status()&8)!=0 && (f.read(0x10002010)&0x8000)!=0 && f.read(0x10002030)==0x1b3);
        const auto actual=f.samples(true);
        for (unsigned y=0;y<16;++y) for (unsigned x=0;x<16;++x)
            check(actual[y*16+x]==128+(field ? y%2 : y/8)*2+x/8);
        for (unsigned i=256;i<320;++i) check(actual[i]==127);
        for (unsigned i=320;i<384;++i) check(actual[i]==130);
        // Clear input while keeping predictors. A second macroblock carries DC
        // state across commands, without taking a reset shortcut.
        check(f.write(0x10002000,0)==Stop::budget); f.acknowledge();
        check((f.read(0x10002010)&0xc000)==0);
        f.destination(512);
        f.upload(stream("10010" "10010" "10010" "10010" "0010" "0010"));
        check(f.write(0x10002000,0x28080000)==Stop::budget);
        const auto next=f.samples();
        for (unsigned i=0;i<256;++i) check(next[i]==131);
        for (unsigned i=256;i<320;++i) check(next[i]==127);
        for (unsigned i=320;i<384;++i) check(next[i]==130);
    }
    {
        Fixture f; f.matrix(1); f.destination(512);
        f.upload(stream("1010" "11" "10")); // Only first block; negative inter DC.
        check(f.write(0x10002000,0x20080000)==Stop::budget);
        const auto actual=f.samples();
        for (unsigned y=0;y<16;++y) for (unsigned x=0;x<16;++x) check(actual[y*16+x]==(y<8 && x<8 ? -3 : 0));
        for (unsigned i=256;i<384;++i) check(actual[i]==0);
        check(((f.read(0x10002010)>>8)&63)==32);
    }
    {
        Fixture f; f.matrix(0); f.destination(512,24);
        f.upload(stream("10010" "10010" "10010" "10010" "0010" "0010"));
        check(f.write(0x10002000,0x2c080000)==Stop::budget);
        check((f.platform.completions.status()&8)!=0 && (f.read(0x10002010)&0x80000000)!=0);
        f.platform.completions.acknowledge(8);
        f.destination(896,24);
        check((f.platform.completions.status()&8)!=0 && !(f.read(0x10002010)&0x80000000));
        const auto actual=f.samples();
        check(std::all_of(actual.begin(),actual.end(),[](auto v) { return v==128; }));
    }
    for (const bool alternate:{false,true}) {
        Fixture f; f.matrix(0); f.destination(512);
        check(f.write(0x10002010,alternate ? 0x00300000 : 0)==Stop::budget);
        const auto encoded=alternate ? "1001000110" "1000110" "1000110" "1000110" "000110" "000110" :
                                       "10011010" "10010" "10010" "10010" "0010" "0010";
        f.upload(stream(encoded));
        check(f.write(0x10002000,0x2c080000)==Stop::budget);
        const auto actual=f.samples();
        constexpr int gradient[]{131,130,130,129,127,126,126,125};
        for (unsigned y=0;y<8;++y) for (unsigned x=0;x<8;++x) check(actual[y*16+x]==gradient[alternate ? y : x]);
        check(f.platform.video.decoded_blocks()==1);
    }
    {
        Fixture f; f.matrix(1); f.destination(512);
        check(f.write(0x10002010,0x00800000)==Stop::budget);
        // MPEG-1 extended negative escape: run 0, level -129. Quantized DC
        // is -2071, saturates to -2048 and reconstructs a constant -256 block.
        f.upload(stream("1010" "0000010000001000000001111111" "10"));
        check(f.write(0x10002000,0x20080000)==Stop::budget);
        const auto actual=f.samples();
        for (unsigned y=0;y<8;++y) for (unsigned x=0;x<8;++x) check(actual[y*16+x]==-256);
    }
    {
        Fixture f; f.matrix(0); f.destination(512);
        std::string bits="100";
        for (unsigned i=0;i<50;++i) bits+="000001000000000000000001"; // MPEG-2 escape, run 0, level +1.
        bits+="10" "10010" "10010" "10010" "0010" "0010";
        const auto data=stream(bits);
        check(f.write(0x10002000,0x2c080000)==Stop::budget);
        f.upload(std::span(data).first(16));
        check(f.ram[512]==0xcd && !(f.platform.completions.status()&8));
        for (std::size_t offset=16;offset<data.size();offset+=16) {
            // Model a producer that only refills when the input queue is empty.
            // A partial VLC in the decoder cache must not strand this producer.
            check(((f.read(0x10002020)>>8)&15)==0);
            f.upload(std::span(data).subspan(offset,16));
        }
        check(f.platform.video.decoded_blocks()==1 && (f.platform.completions.status()&8)!=0 && !(f.read(0x10002010)&0x80000000));
        const auto actual=f.samples();
        check(actual[0]!=actual[1]);
    }
    {
        Fixture f; f.destination(512);
        std::array<std::uint8_t,16> invalid{}; f.upload(invalid);
        check(f.write(0x10002000,0x20080000)==Stop::budget);
        check((f.read(0x10002010)&0x4000)!=0 && f.platform.video.decoded_blocks()==0);
        check(f.ram[512]==0xcd && !(f.platform.completions.status()&8));
    }
    {
        Fixture f;
        check(f.write(0x1000b010,0)==Stop::budget && f.write(0x1000b020,48)==Stop::budget);
        check(f.write(0x1000b000,256)==Stop::code_write && !(f.read(0x1000b000)&256));
        check(f.write(0x1000b010,0x80003ff0)==Stop::budget);
        check(f.write(0x1000b000,256)==Stop::memory_fault && !(f.platform.completions.status()&8));
    }
    {
        Fixture f;
        const std::uint32_t descriptors[]{0x30000010,512,0,0,0x70000008,0,0,0};
        std::memcpy(f.ram.data()+256,descriptors,sizeof(descriptors));
        std::fill_n(f.ram.data()+288,128,0xa5);
        for (unsigned i=0;i<256;++i) f.ram[512+i]=static_cast<std::uint8_t>(i);
        check(f.write(0x1000b430,256)==Stop::budget && f.write(0x1000b400,0x104)==Stop::budget);
        check(f.read(0x1000b410)==640 && f.read(0x1000b420)==8 && f.read(0x1000b430)==272);
        const auto pause=[&] {
            check(f.write(0x1000f590,0x11201)==Stop::budget);
            check(f.write(0x1000b400,f.read(0x1000b400)&~256U)==Stop::budget);
            check(!f.platform.video.submission_pending() && !(f.platform.completions.status()&16));
        };
        const auto resume=[&] {
            check(f.write(0x1000b400,f.read(0x1000b400)|256U)==Stop::budget);
            check(f.write(0x1000f590,0x1201)==Stop::budget);
        };
        pause(); resume();
        for (unsigned block=0;block<3;++block) {
            check(f.write(0x10002000,0x50000000)==Stop::budget);
            check(f.platform.video.matrix(0)[0]==block*64 && f.platform.video.matrix(0)[63]==block*64+63);
        }
        check(f.read(0x1000b410)==352 && f.read(0x1000b420)==4 && f.read(0x1000b430)==272 &&
              (f.read(0x1000b400)>>28)==7);
        pause(); resume();
        check(f.write(0x10002000,0x50000000)==Stop::budget);
        check(f.platform.video.matrix(0)[0]==192 && f.platform.video.matrix(0)[63]==255 && (f.platform.completions.status()&16));
        for (unsigned block=0;block<2;++block) {
            check(f.write(0x10002000,0x50000000)==Stop::budget);
            check(std::all_of(f.platform.video.matrix(0).begin(),f.platform.video.matrix(0).end(),[](auto v) { return v==0xa5; }));
        }
    }
    {
        Fixture f; f.destination(512,128);
        // Two complete, original YCbCr 4:2:0 blocks: black and saturated red.
        std::fill_n(f.ram.data()+3072,256,16);
        std::fill_n(f.ram.data()+3328,128,128);
        std::fill_n(f.ram.data()+3456,256,81);
        std::fill_n(f.ram.data()+3712,64,90);
        std::fill_n(f.ram.data()+3776,64,240);
        check(f.write(0x10002000,0x90800001)==Stop::budget); f.acknowledge();
        check(f.write(0x1000b410,3072)==Stop::budget && f.write(0x1000b420,48)==Stop::budget);
        check(f.write(0x1000b400,256)==Stop::budget && !(f.platform.completions.status()&24));
        check(f.write(0x10002000,0x70000002)==Stop::budget);
        check(f.platform.video.converted_blocks()==2 && (f.platform.completions.status()&24)==24);
        check((f.read(0x10002010)&0x80000000)==0 && f.read(0x1000b020)==0);
        for (unsigned i=0;i<1024;++i) check(f.ram[512+i]==0);
        for (unsigned i=0;i<256;++i) {
            check(f.ram[1536+i*4]==254 && f.ram[1537+i*4]==0 && f.ram[1538+i*4]==0 && f.ram[1539+i*4]==128);
        }
    }
}
