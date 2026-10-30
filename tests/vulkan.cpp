#include <nt/vulkan_renderer.hpp>
#include <nt/native_drawing.hpp>
#include <iostream>
#include <array>
#include <vector>
#include <stdexcept>

int main() {
    try {
        nt::graphics::VulkanRenderer gpu(nullptr,true);
        gpu.synchronize();
        nt::graphics::NativeDrawing native;
        nt::graphics::NativeDrawing::Packet packet{{0x8007,0x10000000,14,0}};
        const auto ad=[&](unsigned reg,std::uint64_t value) {
            packet.push_back({std::uint32_t(value),std::uint32_t(value>>32),reg,0});
        };
        ad(0x1a,1); ad(0,6); ad(0x4c,1ULL<<16);
        ad(0x40,(3ULL<<16)|(3ULL<<48)); ad(1,0x80604020);
        ad(5,0); ad(5,32ULL|(32ULL<<16));
        native.submit(gpu,std::array{packet});
        for (unsigned pixel=0;pixel<4;++pixel)
            if (native.pixels()[pixel]!=0x80604020) throw std::runtime_error("Native GIF batch did not render Vulkan sprite");
        if (native.pixels()[4]) throw std::runtime_error("Native GIF sprite exceeded its coverage");
        const nt::graphics::NativeDrawing::Packet packed{
            {0x8001,0x30000000,0x551,0}, {0x11,0x22,0x33,0x80}, {32,0,0,0}, {64,32,0,0}};
        native.submit(gpu,std::array{packed});
        for (unsigned pixel=4;pixel<8;++pixel)
            if (native.pixels()[pixel]!=0x80332211) throw std::runtime_error("Packed GIF attributes failed");
        const nt::graphics::NativeDrawing::Packet reglist{
            {0x8001,0x34000000,0x551,0}, {0x80776655,0,32U<<16,0}, {32U|(64U<<16),0,0xdeadbeef,0}};
        native.submit(gpu,std::array{reglist});
        for (unsigned pixel=16;pixel<20;++pixel)
            if (native.pixels()[pixel]!=0x80776655) throw std::runtime_error("Register-list GIF attributes failed");
        const auto snapshot=native.pixels();
        auto invalid=packet; invalid[1][2]=0x70;
        auto changed=packet; changed[5][0]=0x12345678;
        bool rejected=false;
        try { native.submit(gpu,std::array{changed,invalid}); }
        catch (const std::invalid_argument&) { rejected=true; }
        if (!rejected || native.pixels()!=snapshot) throw std::runtime_error("Invalid graphics batch changed pixels");
        auto truncated=packet; truncated.pop_back(); rejected=false;
        try { native.submit(gpu,std::array{truncated}); }
        catch (const std::invalid_argument&) { rejected=true; }
        if (!rejected || native.pixels()!=snapshot) throw std::runtime_error("Truncated graphics batch changed pixels");
        const auto transfer_setup=[](unsigned format,unsigned width,unsigned height) {
            nt::graphics::Packet setup{{0x8004,0x10000000,14,0}};
            const auto put=[&](unsigned reg,std::uint64_t value) {
                setup.push_back({std::uint32_t(value),std::uint32_t(value>>32),reg,0});
            };
            put(0x50,(1ULL<<48)|(std::uint64_t(format)<<56)); put(0x51,0);
            put(0x52,width|(std::uint64_t(height)<<32)); put(0x53,0);
            return setup;
        };
        const nt::graphics::Packet rgba{{0x8001,0x08000000,0,0},{0x11223344,0x55667788,0x99aabbcc,0xddeeff00}};
        const auto setup32=transfer_setup(0,2,2);
        native.submit(gpu,std::array{packet,setup32,rgba});
        for (unsigned pixel=0;pixel<4;++pixel)
            if (native.pixels()[pixel]!=rgba[1][pixel]) throw std::runtime_error("RGBA image upload ordering failed");
        native.submit(gpu,std::array{setup32,rgba,packet});
        for (unsigned pixel=0;pixel<4;++pixel)
            if (native.pixels()[pixel]!=0x80604020) throw std::runtime_error("Draw after image upload lost ordering");
        const auto before_rgb=native.pixels();
        const auto setup24=transfer_setup(1,6,1);
        const nt::graphics::Packet rgb_first{{0x8001,0x08000000,0,0},{0x04030201,0x08070605,0x0c0b0a09,0x100f0e0d}};
        const nt::graphics::Packet rgb_last{{0x8001,0x08000000,0,0},{0xdead1211,0xffffffff,0xffffffff,0xffffffff}};
        native.submit(gpu,std::array{setup24,rgb_first});
        const auto partial_rgb=native.pixels(); rejected=false;
        try { native.submit(gpu,std::array{rgb_last,invalid}); }
        catch (const std::invalid_argument&) { rejected=true; }
        if (!rejected || native.pixels()!=partial_rgb) throw std::runtime_error("Failed image continuation changed state");
        native.submit(gpu,std::array{rgb_last});
        const std::array<unsigned,6> addresses{0,1,4,5,8,9};
        for (unsigned pixel=0;pixel<6;++pixel) {
            const auto value=(pixel*3+1)|((pixel*3+2)<<8)|((pixel*3+3)<<16);
            if (native.pixels()[addresses[pixel]]!=((before_rgb[addresses[pixel]]&0xff000000U)|value))
                throw std::runtime_error("RGB image continuation or alpha preservation failed");
        }
        const auto complete_rgb=native.pixels(); rejected=false;
        try { native.submit(gpu,std::array{rgb_last}); }
        catch (const std::invalid_argument&) { rejected=true; }
        if (!rejected || native.pixels()!=complete_rgb) throw std::runtime_error("Image overrun was accepted");
        nt::graphics::Packet textured_packet{{0x800a,0x10000000,14,0}};
        const auto texture_ad=[&](unsigned reg,std::uint64_t value) {
            textured_packet.push_back({std::uint32_t(value),std::uint32_t(value>>32),reg,0});
        };
        const auto texture_description=(1ULL<<14)|(1ULL<<26)|(1ULL<<30)|(1ULL<<34)|(1ULL<<35);
        texture_ad(0x1a,1); texture_ad(0,0x116); texture_ad(0x4c,(1ULL<<16)|1);
        texture_ad(0x40,(3ULL<<16)|(3ULL<<48)); texture_ad(1,0x80808080);
        texture_ad(6,texture_description); texture_ad(3,0); texture_ad(5,0);
        texture_ad(3,32ULL|(32ULL<<16)); texture_ad(5,32ULL|(32ULL<<16));
        native.submit(gpu,std::array{setup32,rgba,textured_packet});
        for (unsigned pixel=0;pixel<4;++pixel)
            if (native.pixels()[2048+pixel]!=rgba[1][pixel]) throw std::runtime_error("Native uploaded texture did not reach Vulkan sampling");
        const auto before_bad_texture=native.pixels();
        auto perspective=textured_packet; perspective[2][0]=0x16; rejected=false;
        try { native.submit(gpu,std::array{perspective}); }
        catch (const std::invalid_argument&) { rejected=true; }
        if (!rejected || native.pixels()!=before_bad_texture) throw std::runtime_error("Unsupported perspective sprite changed pixels");
        // Original index data and a 16-colour palette, uploaded in one batch.
        // A one-pixel PSMT4 sprite selects the low nibble (4) of the first word.
        auto palette_setup=transfer_setup(0,8,2); palette_setup[1][1]|=64;
        nt::graphics::Packet palette_image{{0x8004,0x08000000,0,0}};
        for (unsigned row=0;row<4;++row)
            palette_image.push_back({0x80100000+row*4,0x80100001+row*4,0x80100002+row*4,0x80100003+row*4});
        auto indexed=textured_packet;
        const auto indexed_description=texture_description|(20ULL<<20)|(64ULL<<37)|(3ULL<<56);
        const auto set_palette=[&](unsigned mode) {
            const auto value=indexed_description|(std::uint64_t(mode)<<61);
            indexed[6][0]=std::uint32_t(value); indexed[6][1]=std::uint32_t(value>>32);
        };
        indexed[9][0]=16U|(16U<<16); indexed[10][0]=16U|(16U<<16);
        set_palette(2);
        native.submit(gpu,std::array{setup32,rgba,palette_setup,palette_image,indexed});
        if (native.pixels()[2048]!=0x80100004) throw std::runtime_error("Native indexed texture palette load failed");
        for (std::size_t i=1;i<palette_image.size();++i) for (auto& lane:palette_image[i]) lane+=0x10000;
        set_palette(4);
        native.submit(gpu,std::array{palette_setup,palette_image,indexed});
        if (native.pixels()[2048]!=0x80100004) throw std::runtime_error("Conditional palette load discarded its snapshot");
        set_palette(1);
        native.submit(gpu,std::array{indexed});
        if (native.pixels()[2048]!=0x80110004) throw std::runtime_error("Native palette reload missed updated image data");
        const auto palette_snapshot=native.pixels();
        for (std::size_t i=1;i<palette_image.size();++i) for (auto& lane:palette_image[i]) lane+=0x10000;
        rejected=false;
        try { native.submit(gpu,std::array{palette_setup,palette_image,indexed,invalid}); }
        catch (const std::invalid_argument&) { rejected=true; }
        if (!rejected || native.pixels()!=palette_snapshot) throw std::runtime_error("Rejected palette batch published pixels");
        set_palette(0);
        native.submit(gpu,std::array{indexed});
        if (native.pixels()[2048]!=0x80110004) throw std::runtime_error("Rejected palette batch published palette state");
        auto bad_palette=indexed; bad_palette[6][1]|=(2U<<19)|(1U<<29); rejected=false;
        try { native.submit(gpu,std::array{bad_palette}); }
        catch (const std::invalid_argument&) { rejected=true; }
        if (!rejected || native.pixels()!=palette_snapshot) throw std::runtime_error("Unsupported palette format changed pixels");
        // Indexed uploads preserve neighbouring nibbles and feed the same GPU sampler.
        auto upload_index=transfer_setup(20,1,1);
        upload_index[1][1]|=4U<<16; // Odd pitch 5 maps to two 128-pixel pages.
        struct IndexedPoint { unsigned x,y,nibble; };
        const IndexedPoint points[]={{0,0,0},{1,0,8},{0,1,16},{0,2,65},{0,4,192},
            {0,8,256},{32,0,1024},{0,16,512},{128,0,16384},{0,128,32768}};
        for (const auto point:points) {
            upload_index[2][1]=point.x|(point.y<<16);
            const auto before=native.pixels();
            const nt::graphics::Packet indices{{0x8001,0x08000000,0,0},{0xfffffff7,0xffffffff,0xffffffff,0xffffffff}};
            native.submit(gpu,std::array{upload_index,indices});
            const auto shift=(point.nibble%8)*4,address=point.nibble/8;
            if (native.pixels()[address]!=((before[address]&~(15U<<shift))|(7U<<shift)))
                throw std::runtime_error("Indexed upload changed adjacent nibbles or used wrong layout");
            // Sample the uploaded point; the large destination avoids texture/palette storage.
            auto sample=indexed;
            sample[3][0]=(1U<<16)|16;
            const auto description=(4ULL<<14)|(20ULL<<20)|(8ULL<<26)|(8ULL<<30)|
                (1ULL<<34)|(1ULL<<35)|(64ULL<<37)|(3ULL<<56);
            sample[6][0]=std::uint32_t(description); sample[6][1]=std::uint32_t(description>>32);
            sample[7][0]=(point.x*16)|((point.y*16)<<16);
            sample[9][0]=sample[7][0];
            native.submit(gpu,std::array{sample});
            if (native.pixels()[32768]!=0x80110007) throw std::runtime_error("Indexed upload did not reach Vulkan palette sampling");
        }
        auto split_index=transfer_setup(20,33,1);
        const nt::graphics::Packet first_indices{{0x8001,0x08000000,0,0},{0x76543210,0xfedcba98,0x76543210,0xfedcba98}};
        const nt::graphics::Packet last_index{{0x8001,0x08000000,0,0},{0xfffffffa,0xffffffff,0xffffffff,0xffffffff}};
        native.submit(gpu,std::array{split_index,first_indices});
        const auto split_snapshot=native.pixels(); rejected=false;
        try { native.submit(gpu,std::array{last_index,invalid}); }
        catch (const std::invalid_argument&) { rejected=true; }
        if (!rejected || native.pixels()!=split_snapshot) throw std::runtime_error("Indexed continuation rollback failed");
        native.submit(gpu,std::array{last_index}); // x=32 is nibble 1024, word 128.
        if (native.pixels()[128]!=((split_snapshot[128]&~15U)|10U))
            throw std::runtime_error("Indexed odd-length continuation consumed padding or lost final index");
        const auto pixel_draw=[&](std::uint32_t color,unsigned attributes,unsigned test,
            std::uint64_t alpha=0,bool clamp=true,bool per_pixel=false) {
            auto draw=packet;
            draw[2][0]=attributes; draw[5][0]=color;
            draw.resize(draw.size()-2);
            const auto put=[&](unsigned reg,std::uint64_t value) {
                draw.push_back({std::uint32_t(value),std::uint32_t(value>>32),reg,0});
            };
            put(0x47,test); put(0x42,alpha); put(0x46,clamp); put(0x49,per_pixel);
            put(5,0); put(5,32ULL|(32ULL<<16));
            draw[0][0]=0x8000U|unsigned(draw.size()-1);
            return draw;
        };
        const auto background=pixel_draw(0x80202020,6,0);
        constexpr bool alpha_equal_pass[]{false,true,false,true,true,true,false,false};
        for (unsigned mode=0;mode<8;++mode) {
            native.submit(gpu,std::array{background,pixel_draw(0x40102030,6,1|(mode<<1)|(64<<4))});
            if (native.pixels()[0]!=(alpha_equal_pass[mode] ? 0x40102030U : 0x80202020U))
                throw std::runtime_error("Native alpha comparison differs");
        }
        for (unsigned failure=0;failure<4;++failure) {
            native.submit(gpu,std::array{background,pixel_draw(0x40102030,6,1|(failure<<12))});
            const unsigned expected=failure==1 ? 0x40102030 : failure==3 ? 0x80102030 : 0x80202020;
            if (native.pixels()[0]!=expected) throw std::runtime_error("Native alpha failure action differs");
        }
        for (unsigned match=0;match<2;++match) {
            native.submit(gpu,std::array{background,pixel_draw(0x40102030,6,0x4000|(match<<15))});
            if (native.pixels()[0]!=(match ? 0x40102030U : 0x80202020U))
                throw std::runtime_error("Native destination alpha test differs");
        }
        native.submit(gpu,std::array{background,pixel_draw(0x40808080,0x46,0,0x44)});
        if (native.pixels()[0]!=0x40505050) throw std::runtime_error("Native source-alpha blend differs");
        native.submit(gpu,std::array{background,pixel_draw(0x40808080,0x46,0,(64ULL<<32)|0x64)});
        if (native.pixels()[0]!=0x40505050) throw std::runtime_error("Native fixed-alpha blend differs");
        native.submit(gpu,std::array{pixel_draw(0x80010101,6,0),pixel_draw(0x40000000,0x46,0,0x44)});
        if (native.pixels()[0]!=0x40000000) throw std::runtime_error("Negative blend rounding differs");
        for (const bool clamp:{false,true}) {
            native.submit(gpu,std::array{background,pixel_draw(0x80f0f0f0,0x46,0,(128ULL<<32)|0x68,clamp)});
            if (native.pixels()[0]!=(clamp ? 0x80ffffffU : 0x80101010U))
                throw std::runtime_error("Native blend clamp/wrap differs");
        }
        native.submit(gpu,std::array{background,pixel_draw(0x40808080,0x46,0,0x44,true,true)});
        if (native.pixels()[0]!=0x40808080) throw std::runtime_error("Per-pixel blend selection differs");
        const auto before_blend_failure=native.pixels(); rejected=false;
        try { native.submit(gpu,std::array{background,pixel_draw(0,0x46,0,3)}); }
        catch (const std::invalid_argument&) { rejected=true; }
        if (!rejected || native.pixels()!=before_blend_failure) throw std::runtime_error("Invalid blend batch changed pixels");
        auto frame24=pixel_draw(0x40808080,0x46,0x4000,0x54);
        frame24[3][0]|=1U<<24;
        native.submit(gpu,std::array{background,frame24});
        if (native.pixels()[0]!=0x80808080) throw std::runtime_error("24-bit frame did not ignore DATE/use fixed destination alpha");
        auto alias_draw=pixel_draw(0x40808080,0x46,0,0x44);
        alias_draw[4][0]=127U<<16; alias_draw.back()[0]=2048U|(32U<<16);
        const auto before_alias=native.pixels(); rejected=false;
        try { native.submit(gpu,std::array{background,alias_draw}); }
        catch (const std::runtime_error&) { rejected=true; }
        if (!rejected || native.pixels()!=before_alias) throw std::runtime_error("Aliased destination blend was not atomic/rejected");
        std::vector<std::uint32_t> memory(1024*1024,0x12345678), expected=memory;
        // Independent block/column lookup, including page crossing and VRAM wrap.
        constexpr unsigned blocks[4][8]={{0,1,4,5,16,17,20,21},{2,3,6,7,18,19,22,23},
            {8,9,12,13,24,25,28,29},{10,11,14,15,26,27,30,31}};
        constexpr unsigned columns[2][8]={{0,1,4,5,8,9,12,13},{2,3,6,7,10,11,14,15}};
        for (const auto draw : {nt::graphics::Sprite{3,1,130,65,0,3,0xaabbccdd,0xff00ff00},
                               nt::graphics::Sprite{0,0,128,64,16352,2,0x98765432,0x00ff00ff},
                               nt::graphics::Sprite{0,0,256,128,0,1,0x11223344,0xff000000}}) {
            for (unsigned y=draw.top;y<draw.bottom;++y) for (unsigned x=draw.left;x<draw.right;++x) {
                const unsigned address=(draw.base*64+((y/32)*draw.pitch+x/64)*2048+
                    blocks[(y%32)/8][(x%64)/8]*64+((y%8)/2)*16+columns[y%2][x%8])&0xfffff;
                expected[address]=(expected[address]&draw.mask)|(draw.color&~draw.mask);
            }
            gpu.sprite(memory,draw);
            gpu.synchronize();
            if (memory!=expected) throw std::runtime_error("GPU sprite pixels differ from independent reference");
        }
        // A 2x2 original texture, with known column addresses, rendered into
        // another page and then over itself to exercise snapshot isolation.
        const std::uint32_t texels[]{0x20102030,0x40506070,0x8090a0b0,0xc0d0e0f0};
        nt::graphics::Sprite textured{0,0,2,2,32,1,0x80808080,0};
        textured.flags=13; textured.texture_pitch=1;
        textured.texture_width=textured.texture_height=2;
        textured.dx=textured.dy=textured.du=textured.dv=32;
        for (unsigned mode=0;mode<4;++mode) {
            std::fill(memory.begin(),memory.end(),0);
            std::copy(std::begin(texels),std::end(texels),memory.begin());
            textured.clamp_u=textured.clamp_v=mode;
            textured.min_u=textured.min_v=0; textured.max_u=textured.max_v=1;
            textured.u0=textured.v0=32;
            gpu.sprite(memory,textured);
            for(unsigned i=0;i<4;++i) {
                const unsigned expected_index=mode==0?i:3;
                if(memory[2048+i]!=texels[expected_index]) throw std::runtime_error("Vulkan texture wrap/clamp mismatch");
            }
        }
        textured.clamp_u=textured.clamp_v=0; textured.u0=textured.v0=0;
        textured.base=0; textured.du=-32; textured.u0=16;
        std::copy(std::begin(texels),std::end(texels),memory.begin());
        gpu.sprite(memory,textured);
        for(unsigned i=0;i<4;++i) if(memory[i]!=texels[i^1]) throw std::runtime_error("Overlapping mirrored texture read raced output");
        textured.base=32; textured.du=32; textured.u0=0; textured.flags=1;
        textured.color=0x40404040; // MODULATE halves RGB; TCC=0 keeps vertex alpha.
        std::copy(std::begin(texels),std::end(texels),memory.begin());
        gpu.sprite(memory,textured);
        if(memory[2048]!=0x40081018) throw std::runtime_error("Texture modulation or TCC differs");
        textured.flags=15; textured.texa=0x8080; // 24-bit DECAL, TCC and AEM.
        memory[0]=0xff000000; memory[1]=0x00112233;
        gpu.sprite(memory,textured);
        if(memory[2048]!=0 || memory[2049]!=0x80112233) throw std::runtime_error("24-bit TEXA expansion differs");
        std::array<std::uint32_t,16> palette{};
        for(unsigned i=0;i<16;++i)palette[i]=0xff102030+i;
        textured={0,0,1,1,32,1,0x80808080,0};
        textured.flags=77; textured.texture_pitch=4;
        textured.texture_width=textured.texture_height=256;
        textured.dx=textured.dy=16;
        struct Sample {unsigned x,y,nibble;};
        const Sample samples[]={{0,0,0},{1,0,8},{0,1,16},{0,2,65},{0,4,192},{0,8,256},
            {32,0,1024},{0,16,512},{128,0,16384},{0,128,32768}};
        for(unsigned i=0;i<std::size(samples);++i) {
            std::fill(memory.begin(),memory.end(),0);
            const auto s=samples[i];
            memory[s.nibble/8]=(i+1)<<((s.nibble%8)*4);
            textured.u0=s.x*16; textured.v0=s.y*16;
            gpu.sprite(memory,textured,palette);
            if(memory[2048]!=palette[i+1])throw std::runtime_error("PSMT4 block/column/page addressing differs");
        }
        // Exercise every four-bit bilinear weight in both axes, for direct and
        // indexed textures. Filtering must happen after palette expansion.
        for (bool indexed : {false,true}) {
            std::fill(memory.begin(),memory.end(),0);
            for(unsigned i=0;i<4;++i) {
                constexpr unsigned colors[]{7,42,79,113};
                const auto c=colors[i];
                palette[i]=c*0x01010101U;
                memory[i]=indexed ? i : palette[i];
            }
            textured={0,0,16,16,32,1,0x80808080,0};
            textured.flags=13|128|(indexed?64:0);
            textured.texture_pitch=2; textured.texture_width=textured.texture_height=2;
            textured.dx=textured.dy=256; textured.du=textured.dv=16;
            textured.u0=textured.v0=8;
            gpu.sprite(memory,textured,palette);
            for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
                const unsigned top=(7*(16-x)+42*x)/16, bottom=(79*(16-x)+113*x)/16;
                const unsigned expected_color=((top*(16-y)+bottom*y)/16)*0x01010101U;
                const unsigned destination=2048+blocks[y/8][x/8]*64+((y%8)/2)*16+columns[y%2][x%8];
                if(memory[destination]!=expected_color)throw std::runtime_error("Bilinear weight or palette expansion order differs");
            }
        }
        if (gpu.validation_errors()) throw std::runtime_error("Vulkan validation reported errors");
        std::cout << "Vulkan sprite readback verified on " << gpu.device_name() << "; validation errors=0\n";
    } catch (const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
