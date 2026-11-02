#pragma once
#include <cstdint>

using Word=std::uint32_t;
using Wide=std::uint64_t;
inline constexpr Word result_magic=0x41503254; // AP2T
struct Result {
    Word magic,version,case_id,complete,count;
    Word values[16];
};
extern "C" volatile Result anyps2_test_result;
extern volatile Word test_input;
void record(Word value);
void test_body();
#define TEST_NOINLINE __attribute__((noinline))
