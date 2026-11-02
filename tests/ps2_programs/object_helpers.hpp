#pragma once
#include "protocol.hpp"

struct Primary {
    Word bias;
    explicit Primary(Word value) : bias(value) {}
    virtual Word evaluate(Word value) { return value + bias; }
};
struct Secondary {
    Word scale;
    explicit Secondary(Word value) : scale(value) {}
    virtual Word evaluate_secondary(Word value) { return value * scale; }
};
struct Combined final : Primary, Secondary {
    Word offset;
    Combined(Word bias, Word scale, Word extra)
        : Primary(bias), Secondary(scale), offset(extra) {}
    Word evaluate(Word value) override { return value + bias + offset; }
    Word evaluate_secondary(Word value) override { return value * scale + bias + offset; }
};
Word call_primary(Primary* object, Word value);
Word call_secondary(Secondary* object, Word value);
