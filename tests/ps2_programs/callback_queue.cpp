#include "callback_helpers.hpp"

struct Job { Callback callback; Word value; };
struct Queue {
    Job jobs[4]{};
    Word read = 0;
    Word write = 0;
};
TEST_NOINLINE bool submit(Queue& queue, Callback callback, Word value) {
    if (queue.write == 4) return false;
    queue.jobs[queue.write++] = {callback, value};
    return true;
}
TEST_NOINLINE bool take(Queue& queue, Job& job) {
    if (queue.read == queue.write) return false;
    job = queue.jobs[queue.read++];
    return true;
}
void test_body() {
    Queue queue;
    const Word input = test_input;
    record(submit(queue, add_callback, input));
    record(submit(queue, multiply_callback, input));
    Job job{};
    Word total = 0;
    Word order = 0;
    while (take(queue, job)) {
        const Word value = call_callback(job.callback, job.value);
        total += value;
        order = order * 100 + value;
        if (queue.read == 1) submit(queue, add_callback, input + 1);
    }
    record(total);
    record(order);
    record(queue.read);
    record(take(queue, job));
}
