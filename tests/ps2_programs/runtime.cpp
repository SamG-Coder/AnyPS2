#include "protocol.hpp"

extern "C" {
volatile Result anyps2_test_result{};
}
volatile Word test_input=7;

void record(Word value) {
    const auto index=anyps2_test_result.count;
    if (index<16) {
        anyps2_test_result.values[index]=value;
        anyps2_test_result.count=index+1;
    }
}
extern "C" void test_entry() {
    anyps2_test_result.magic=result_magic;
    anyps2_test_result.version=1;
    anyps2_test_result.case_id=TEST_CASE_ID;
    anyps2_test_result.complete=0;
    anyps2_test_result.count=0;
    test_body();
    // Publish completion after the observations. The test never calls firmware.
    asm volatile("sync" ::: "memory");
    anyps2_test_result.complete=1;
}
