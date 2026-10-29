#pragma once
#include <nt/runtime.hpp>
#include <memory>
#include <nt/vulkan_renderer.hpp>

namespace nt::hardware {
// Privileged registers and diagnostic scanout timing; no GIF renderer.
class Gs {
public:
    struct Vertex {
        std::uint16_t x=0, y=0;
        std::uint32_t z=0;
        std::uint8_t fog=0;
        std::uint64_t rgbaq=0, st=0, uv=0;
    };
    unsigned pending_vertices() const { return vertex_count_; }
    const Vertex& pending_vertex(unsigned index) const { return vertices_.at(index); }
    static bool maps(std::uint32_t address) {
        return address >= 0x12000000U && address < 0x12002000U;
    }
    std::uint64_t read(std::uint32_t address, unsigned size) const;
    void write(std::uint32_t address, unsigned size, std::uint64_t value);
    void set_crt(std::uint32_t interlace, std::uint32_t mode, std::uint32_t frame);
    void advance_cycles(std::uint64_t cycles);
    void write_command(unsigned reg, std::uint64_t value);
    void upload(runtime::Register value);
    std::uint32_t local_word(unsigned word) const { return local_->at(word); }
    std::uint32_t palette_color(unsigned index) const { return palette_.at(index); }
    std::uint64_t command_register(unsigned reg) const { return drawing_.at(reg); }
    std::uint32_t take_interrupts() { const auto result = interrupts_; interrupts_ = 0; return result; }
private:
    // Configuration transactions share storage; pixel writers detach on mutation.
    std::shared_ptr<std::vector<std::uint32_t>> local_ =
        std::make_shared<std::vector<std::uint32_t>>(1024*1024);
    struct Upload {
        unsigned base=0, pitch=0, x=0, y=0, width=0, height=0, pixel=0, bits=0;
        std::uint64_t pending=0;
        unsigned pending_bits=0;
    } upload_;
    static unsigned pixel_word(unsigned base, unsigned pitch, unsigned x, unsigned y);
    std::array<std::uint64_t,15> display_{};
    std::array<std::uint64_t,128> drawing_{};
    std::array<std::uint32_t,256> palette_{};
    std::array<unsigned,2> palette_base_{};
    void texture_register(unsigned reg, std::uint64_t value);
    std::array<Vertex,3> vertices_{};
    unsigned vertex_count_=0;
    void submit_vertex(unsigned reg, std::uint64_t value);
    void draw_sprite(const Vertex& first, const Vertex& last);
    void writable_local();
    std::shared_ptr<graphics::VulkanRenderer> renderer_;
    std::uint64_t signal_label_ = 0;
    std::uint32_t csr_ = 0x551b4000, imr_ = 0x7f00;
    std::uint32_t mode_ = 0, interrupts_ = 0;
    std::uint64_t phase_ = 0;
    unsigned half_line_ = 0;
    bool horizontal_half_ = false;
    void event(std::uint32_t flag);
    static unsigned offset(std::uint32_t address, unsigned size);
};
}
