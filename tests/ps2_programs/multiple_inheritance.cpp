#include "object_helpers.hpp"

void test_body() {
    const Word input = test_input;
    Combined combined(13, 3, 5);
    Secondary ordinary(2);
    record(call_primary(&combined, input));
    record(call_secondary(&combined, input));
    record(call_secondary(&ordinary, input));
    Secondary* copied = &combined;
    record(call_secondary(copied, input + 1));
    record(static_cast<void*>(static_cast<Primary*>(&combined)) != static_cast<void*>(copied));
}
