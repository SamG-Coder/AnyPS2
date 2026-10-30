#include <nt/ee_vu_launch.hpp>
#include <nt/native_drawing.hpp>
#include <iostream>

extern "C" const nt::vu::Block* nt_vu_graphics_end_blocks();
extern "C" std::uint32_t nt_vu_graphics_end_count();
extern "C" const std::uint8_t* nt_vu_graphics_end_image();
extern "C" std::uint32_t nt_vu_graphics_end_size();

int main() {
    try {
        nt::vu::Binding binding(nt::vu::Program(1,{nt_vu_graphics_end_blocks(),nt_vu_graphics_end_count()}),0,
            {nt_vu_graphics_end_image(),nt_vu_graphics_end_size()});
        if (!binding.upload(1,std::array{nt::vu::UploadView{0,{nt_vu_graphics_end_image(),nt_vu_graphics_end_size()}}}))
            throw std::runtime_error("Synthetic graphics program binding failed");
        struct Consumer {
            nt::graphics::VulkanRenderer renderer{nullptr,true};
            nt::graphics::NativeDrawing drawing;
        } consumer;
        nt::ee::NativeVuLaunch launch(binding);
        launch.graphics={&consumer,[](void* raw,std::span<const nt::vu::GraphicsPacket> packets) {
            auto& sink=*static_cast<Consumer*>(raw);
            sink.drawing.submit(sink.renderer,packets); return true;
        }};
        launch.state.data[0]={0x8007,0x10000000,14,0};
        unsigned cursor=1;
        const auto ad=[&](unsigned reg,std::uint64_t value) {
            launch.state.data[cursor++]={std::uint32_t(value),std::uint32_t(value>>32),reg,0};
        };
        ad(0x1a,1); ad(0,6); ad(0x4c,1ULL<<16);
        ad(0x40,(3ULL<<16)|(3ULL<<48)); ad(1,0x80402010);
        ad(5,0); ad(5,32ULL|(32ULL<<16));
        const nt::ee::NativeLaunchView request{0,0,0};
        if (launch.submit(1,{},&request)!=nt::ee::Stop::budget || launch.state.integers[0]!=1 ||
            launch.state.graphics_pending || launch.last_result.blocks!=1)
            throw std::runtime_error("Compiled VU graphics launch did not complete");
        for (unsigned pixel=0;pixel<4;++pixel)
            if (consumer.drawing.pixels()[pixel]!=0x80402010)
                throw std::runtime_error("Compiled VU packet did not produce expected GPU pixels");
        if (consumer.drawing.pixels()[4] || consumer.renderer.validation_errors())
            throw std::runtime_error("Unexpected coverage or Vulkan validation failure");
        nt::ee::NativePacketSetup direct;
        direct.graphics=launch.graphics;
        std::vector<std::uint8_t> transport;
        const auto word=[&](std::uint32_t value) {
            for (unsigned byte=0;byte<4;++byte) transport.push_back(static_cast<std::uint8_t>(value>>(byte*8)));
        };
        const auto payload=[&]() {
            for (unsigned index=0;index<cursor;++index) for (const auto lane:launch.state.data[index]) word(lane);
        };
        launch.state.data[5][0]=0x80706050;
        word(0x0700002a); word(0x50000000|cursor); payload();
        if (direct.consume(1,transport,false)!=nt::ee::Stop::budget ||
            direct.read(0x10003c30).value!=42 || consumer.drawing.pixels()[0]!=0x80706050)
            throw std::runtime_error("VIF DIRECT did not reach native Vulkan drawing");
        const auto image=consumer.drawing.pixels();
        transport.clear(); word(0x0700002b); word(0x51000000|cursor); payload(); word(0x7f000000);
        if (direct.consume(1,transport,false)!=nt::ee::Stop::unsupported_system ||
            direct.read(0x10003c30).value!=42 || consumer.drawing.pixels()!=image)
            throw std::runtime_error("Invalid DIRECT batch published partial configuration or drawing");
        transport.clear(); word(0x50000000);
        if (direct.consume(1,transport,false)!=nt::ee::Stop::invalid_state || consumer.drawing.pixels()!=image)
            throw std::runtime_error("Encoded-zero DIRECT accepted truncated payload");
        launch.state.data[5][0]=0x80908070;
        transport.clear(); word(0x51000000|cursor); payload();
        if (direct.consume(1,transport,false)!=nt::ee::Stop::budget || consumer.drawing.pixels()[0]!=0x80908070)
            throw std::runtime_error("Serialized DIRECTHL failed");
        launch.state.data[5][0]=0x80201008;
        transport.clear(); payload();
        direct.write(0x10003000,8);
        if (direct.consume(2,transport,false)!=nt::ee::Stop::unsupported_system)
            throw std::runtime_error("Paused graphics accepted a native submission");
        direct.write(0x10003000,0);
        if (direct.consume(2,transport,false)!=nt::ee::Stop::budget || consumer.drawing.pixels()[0]!=0x80201008)
            throw std::runtime_error("Native GIF submission did not reach Vulkan");
        if (consumer.renderer.validation_errors()) throw std::runtime_error("Native transfers caused Vulkan validation errors");
        std::cout<<"Compiled VU to Vulkan sprite integration passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
