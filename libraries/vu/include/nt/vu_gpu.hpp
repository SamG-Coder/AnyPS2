#pragma once
#include <nt/vu_native.hpp>
#include <array>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

namespace nt::vu::gpu {
// Word layout of State in Vulkan storage buffers, shared by the AOT kernel
// generator and the host. Every 32-bit field keeps its State byte offset / 4
// (data memory removed); each 16-bit field gets its own zero-extended word
// after them, so kernels never need sub-word stores. Data memory follows the
// register words, four words per row.
inline constexpr std::size_t data_begin=offsetof(State,data);
inline constexpr std::size_t data_bytes=sizeof(State{}.data);
inline constexpr std::size_t data_end=data_begin+data_bytes;
inline constexpr std::size_t full_words=(sizeof(State)-data_bytes)/4;
inline constexpr std::array<std::size_t,19> half_fields{
    offsetof(State,integers)+0,offsetof(State,integers)+2,offsetof(State,integers)+4,offsetof(State,integers)+6,
    offsetof(State,integers)+8,offsetof(State,integers)+10,offsetof(State,integers)+12,offsetof(State,integers)+14,
    offsetof(State,integers)+16,offsetof(State,integers)+18,offsetof(State,integers)+20,offsetof(State,integers)+22,
    offsetof(State,integers)+24,offsetof(State,integers)+26,offsetof(State,integers)+28,
    offsetof(State,delayed_integer),offsetof(State,delayed_value),offsetof(State,input_top),offsetof(State,input_itop)};
inline constexpr std::size_t register_words=full_words+half_fields.size();
inline constexpr std::size_t data_words=data_bytes/4;
inline constexpr std::size_t state_words=register_words+data_words;
static_assert((sizeof(State)-data_bytes)%4==0 && data_begin%4==0);

// Register word of a 32-bit field at a State byte offset outside data memory.
inline std::size_t word(std::size_t offset) {
    if (offset%4 || (offset>=data_begin && offset<data_end) || offset>=sizeof(State))
        throw std::invalid_argument("Invalid VU GPU state word offset");
    return (offset<data_begin ? offset : offset-data_bytes)/4;
}
// Register word of a 16-bit field at a State byte offset.
inline std::size_t half_word(std::size_t offset) {
    for (std::size_t i=0;i<half_fields.size();++i) if (half_fields[i]==offset) return full_words+i;
    throw std::invalid_argument("Invalid VU GPU 16-bit state field");
}

inline void pack(const State& state,std::span<std::uint32_t,state_words> words) {
    const auto* bytes=reinterpret_cast<const unsigned char*>(&state);
    std::memcpy(words.data(),bytes,data_begin);
    std::memcpy(words.data()+data_begin/4,bytes+data_end,sizeof(State)-data_end);
    for (std::size_t i=0;i<half_fields.size();++i) {
        std::uint16_t value; std::memcpy(&value,bytes+half_fields[i],2);
        words[full_words+i]=value;
    }
    std::memcpy(words.data()+register_words,state.data.data(),data_bytes);
}
inline void unpack(std::span<const std::uint32_t,state_words> words,State& state) {
    auto* bytes=reinterpret_cast<unsigned char*>(&state);
    std::memcpy(bytes,words.data(),data_begin);
    std::memcpy(bytes+data_end,words.data()+data_begin/4,sizeof(State)-data_end);
    for (std::size_t i=0;i<half_fields.size();++i) {
        const auto value=static_cast<std::uint16_t>(words[full_words+i]);
        std::memcpy(bytes+half_fields[i],&value,2);
    }
    std::memcpy(state.data.data(),words.data()+register_words,data_bytes);
}

// Kernel interface (descriptor set 0). Binding 0: word 0 is the job count,
// then job_words per job. Binding 1: job states (state_words each, at the
// job's state word). Binding 2: XGKICK output, whole GIF packets as
// quadwords in kick order, each job writing from its output word.
// One invocation runs one job from its state's next_pair until the program
// ends, fails or exhausts the block budget. A failure leaves the state
// unspecified (blocks are not transactions on the GPU); the host keeps the
// job's entry state and reports or reruns it with the CPU program.
inline constexpr unsigned workgroup_size=64;
inline constexpr unsigned job_words=8;
enum JobWord : unsigned {
    job_state=0,          // first word of the job's state in binding 1
    job_output=1,         // first word of the job's output in binding 2
    job_output_capacity=2,// quadwords
    job_budget=3,         // native blocks
    job_status=4,         // written: JobStatus
    job_blocks=5,         // written: native blocks executed
    job_output_quadwords=6// written
};
enum class JobStatus : std::uint32_t {
    complete=0, unsupported_numeric=1, unmapped_block=2, budget_exhausted=3,
    unsupported_schedule=4, unsupported_memory=5, unsupported_context=6,
    invalid_packet=7, output_full=8, not_run=0xffffffffU
};
}
