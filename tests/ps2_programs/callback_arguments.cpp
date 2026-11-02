#include "callback_helpers.hpp"

void test_body() {
    const Word input = test_input;
    record(call_callback(add_callback, input));
    record(call_callback(multiply_callback, input));
}
