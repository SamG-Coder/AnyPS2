#pragma once
#include <nt/vu_native.hpp>
#include <nt/graphics_packet.hpp>

namespace nt::vu {
enum class GraphicsResult { no_request, unavailable, invalid_packet, accepted };
struct GraphicsConsumer {
    void* context=nullptr;
    // Must accept the entire immutable packet or leave external state unchanged.
    bool (*submit)(void*,std::span<const graphics::Quadword>)=nullptr;
};
inline GraphicsResult submit_graphics(State& state,GraphicsConsumer consumer) {
    if (!state.graphics_pending) return GraphicsResult::no_request;
    if (!consumer.submit) return GraphicsResult::unavailable;
    std::vector<graphics::Quadword> packet;
    try { packet=graphics::collect_packet(state.data,state.graphics_address); }
    catch (const std::invalid_argument&) { return GraphicsResult::invalid_packet; }
    if (!consumer.submit(consumer.context,packet)) return GraphicsResult::unavailable;
    state.graphics_pending=0;
    return GraphicsResult::accepted;
}
}
