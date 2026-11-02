#include "callback_helpers.hpp"

struct Accumulator { Word sum; Word calls; };
Word collect(void* context, Word value) {
    auto& state = *static_cast<Accumulator*>(context);
    state.sum += value;
    ++state.calls;
    return state.sum;
}
Word forward(void* context, Word value) {
    return call_context(static_cast<ContextCallback*>(context), value + 1);
}
void test_body() {
    Accumulator state{10, 0};
    ContextCallback leaf{collect, &state};
    ContextCallback parent{forward, &leaf};
    record(call_context(&parent, test_input));
    record(call_context(&leaf, 3));
    record(state.sum);
    record(state.calls);
}
