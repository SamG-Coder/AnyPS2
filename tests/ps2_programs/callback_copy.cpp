#include "callback_helpers.hpp"

void test_body() {
    const Word input = test_input;
    CallbackRecord source{11, add_callback};
    CallbackRecord destination{};
    copy_bytes(&destination, &source, sizeof(source));
    record(call_callback(destination.callback, input));

    // Legal character access to a trivially copyable pointer representation.
    // Only call after all bytes of the replacement pointer have been written.
    Callback replacement = multiply_callback;
    auto* target = reinterpret_cast<unsigned char*>(&destination.callback);
    const auto* bytes = reinterpret_cast<const unsigned char*>(&replacement);
    copy_bytes(target + 1, bytes + 1, sizeof(Callback) - 1);
    copy_bytes(target, bytes, 1);
    record(call_callback(destination.callback, input));

    // A neighboring field write must not invalidate the still-valid callback.
    const Word new_tag = 99;
    copy_bytes(&destination.tag, &new_tag, sizeof(new_tag));
    record(call_callback(destination.callback, input + 2));
    record(destination.tag);
}
