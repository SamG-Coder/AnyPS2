#pragma once
#include "protocol.hpp"

using Callback = Word (*)(Word);
struct CallbackRecord {
    Word tag;
    Callback callback;
};
struct ContextCallback {
    Word (*callback)(void*, Word);
    void* context;
};

// Separate translation units prevent whole-function constant propagation from
// removing the ABI and pointer operations that these programs exercise.
Word add_callback(Word value);
Word multiply_callback(Word value);
Word call_callback(Callback callback, Word value);
Word call_stack_callback(Word a, Word b, Word c, Word d, Word e, Word f, Word g,
                         Word h, Callback callback, Word value);
Callback select_callback(Word selection);
void replace_callback(Callback* slot, Callback replacement);
void copy_bytes(void* destination, const void* source, Word size);
Word call_context(const ContextCallback* handler, Word value);
