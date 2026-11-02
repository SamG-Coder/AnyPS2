#include "callback_helpers.hpp"

void test_body() {
    const Word input = test_input;
    Callback callback = add_callback;
    Callback* alias = &callback;
    record(call_callback(*alias, input));
    replace_callback(alias, multiply_callback);
    record(call_callback(callback, input));
    replace_callback(&callback, add_callback);
    record(call_callback(*alias, input + 2));
}
