#include "callback_helpers.hpp"

Callback current_callback = add_callback;
void test_body() {
    const Word input = test_input;
    record(call_callback(current_callback, input));
    replace_callback(&current_callback, multiply_callback);
    record(call_callback(current_callback, input));
    // An earlier copy retains the old target after the global changes again.
    const Callback saved = current_callback;
    replace_callback(&current_callback, add_callback);
    record(call_callback(current_callback, input));
    record(call_callback(saved, input));
}
