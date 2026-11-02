#include "protocol.hpp"

using Callback=Word(*)(Word);
TEST_NOINLINE Word plus(Word value) { return value+13; }
TEST_NOINLINE Word times(Word value) { return value*3; }
TEST_NOINLINE Word invoke(Callback callback,Word value) { return callback(value); }
TEST_NOINLINE Callback choose(Word value) { return value&1 ? plus : times; }
struct Slot { Word key; Callback callback; };
Slot registry[2]{};
TEST_NOINLINE void install(Word index,Word key,Callback callback) {
    registry[index]={key,callback};
}
TEST_NOINLINE Callback lookup(Word key) {
    for (const auto& slot:registry) if (slot.key==key) return slot.callback;
    return nullptr;
}
void test_body() {
    const Word input=test_input;
    record(invoke(choose(input),input));
    install(0,11,plus); install(1,22,times);
    auto callback=lookup(22);
    record(callback ? callback(input) : 0);
    install(1,22,plus);
    callback=lookup(22);
    record(callback ? callback(input) : 0);
    record(lookup(99)==nullptr);
}
