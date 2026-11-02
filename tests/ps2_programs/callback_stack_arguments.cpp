#include "callback_helpers.hpp"

void test_body() {
    const Word input = test_input;
    record(call_stack_callback(1, 2, 3, 4, 5, 6, 7, 8, add_callback, input));
    record(call_stack_callback(1, 2, 3, 4, 5, 6, 7, 8, multiply_callback, input));
}
