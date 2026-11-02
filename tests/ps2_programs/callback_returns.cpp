#include "callback_helpers.hpp"

void test_body() {
    const Word input = test_input;
    record(call_callback(select_callback(input), input));
    record(call_callback(select_callback(input + 1), input));
    Callback saved = select_callback(input);
    record(call_callback(saved, input + 2));
}
