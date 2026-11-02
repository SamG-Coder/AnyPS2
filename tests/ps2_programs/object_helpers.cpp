#include "object_helpers.hpp"

Word call_primary(Primary* object, Word value) { return object->evaluate(value); }
Word call_secondary(Secondary* object, Word value) { return object->evaluate_secondary(value); }
