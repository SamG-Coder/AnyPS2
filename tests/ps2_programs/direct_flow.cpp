#include "protocol.hpp"

TEST_NOINLINE Word adjust(Word value, Word selection) {
    return selection & 1 ? value + 11 : value * 2;
}
TEST_NOINLINE Word accumulate(Word seed, Word count) {
    for (Word n = 0; n < count; ++n) seed = adjust(seed, n);
    return seed;
}
void test_body() {
    const Word input = test_input;
    record(adjust(input, input));
    record(adjust(input, input + 1));
    record(accumulate(input, 4));
    record(accumulate(input, 0));
}
