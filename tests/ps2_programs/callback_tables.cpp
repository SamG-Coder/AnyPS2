#include "callback_helpers.hpp"

CallbackRecord handlers[]{{11, add_callback}, {22, multiply_callback}};
void test_body() {
    const Word input = test_input;
    const Word index = input & 1;
    record(call_callback(handlers[index].callback, input));
    Word total = 0;
    for (const auto& handler : handlers) total += call_callback(handler.callback, input);
    record(total);
    replace_callback(&handlers[index].callback, add_callback);
    record(call_callback(handlers[index].callback, input));
    record(handlers[index].tag);
}
