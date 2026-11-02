#include "protocol.hpp"

extern "C" void optional_hook() __attribute__((weak));

void test_body() {
    // A missing optional symbol links to zero. Its guarded call must remain
    // unreachable; supplying a symbol must still leave a real callable path.
    if (optional_hook) optional_hook();
    record(test_input + 116);
}
