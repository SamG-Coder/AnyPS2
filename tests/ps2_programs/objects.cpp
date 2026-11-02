#include "protocol.hpp"

struct Base {
    Word bias;
    explicit Base(Word value):bias(value) {}
    virtual Word evaluate(Word value) { return value+bias; }
};
struct Scaled final:Base {
    explicit Scaled(Word value):Base(value) {}
    Word evaluate(Word value) override { return value*bias; }
};
TEST_NOINLINE Base* factory(Word selection,Base& first,Base& second) {
    return selection&1 ? &first : &second;
}
TEST_NOINLINE Word invoke_object(Base* object,Word value) {
    return object->evaluate(value);
}
void test_body() {
    Base plus(13); Scaled times(3);
    const Word input=test_input;
    record(invoke_object(factory(input,plus,times),input));
    record(invoke_object(factory(input+1,plus,times),input));
    Base* copied=factory(input+1,plus,times);
    Base** alias=&copied;
    record(invoke_object(*alias,input+2));
}
