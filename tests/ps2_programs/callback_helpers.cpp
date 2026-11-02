#include "callback_helpers.hpp"

Word add_callback(Word value) { return value + 13; }
Word multiply_callback(Word value) { return value * 3; }
Word call_callback(Callback callback, Word value) { return callback(value); }
Word call_stack_callback(Word a, Word b, Word c, Word d, Word e, Word f, Word g,
                         Word h, Callback callback, Word value) {
    return a + b + c + d + e + f + g + h + callback(value);
}
Callback select_callback(Word selection) {
    return selection & 1 ? add_callback : multiply_callback;
}
void replace_callback(Callback* slot, Callback replacement) { *slot = replacement; }
void copy_bytes(void* destination, const void* source, Word size) {
    auto* output = static_cast<unsigned char*>(destination);
    const auto* input = static_cast<const unsigned char*>(source);
    for (Word n = 0; n < size; ++n) output[n] = input[n];
}
Word call_context(const ContextCallback* handler, Word value) {
    return handler->callback(handler->context, value);
}
