#include "tests.h"
#include "core/lock_free_queue.h"
#include <memory>

// LockFreeQueue is SPSC. These tests drive it from one thread, which is a
// legitimate subset of that contract (the producer and consumer just happen to
// be the same thread) and is what makes the behaviour deterministic. The
// memory-ordering guarantees are not what is under test here; the ring
// arithmetic and the full/empty edges are.
void test_queue() {
    // A ring of Capacity slots holds Capacity-1 items: push() considers the
    // queue full when the next tail would land on head, so one slot is always
    // left empty to keep full and empty distinguishable. This is a deliberate
    // property, not an off-by-one — pin it.
    {
        LockFreeQueue<int, 4> q;
        CHECK(q.empty());

        int out = -1;
        CHECK(!q.pop(out));           // empty queue yields nothing

        CHECK(q.push(1));
        CHECK(!q.empty());
        CHECK(q.push(2));
        CHECK(q.push(3));
        CHECK(!q.push(4));            // full at Capacity-1 == 3 items

        CHECK(q.pop(out) && out == 1);  // FIFO
        CHECK(q.pop(out) && out == 2);
        CHECK(q.pop(out) && out == 3);
        CHECK(!q.pop(out));
        CHECK(q.empty());
    }

    // Wrap-around: cycling far past Capacity must not corrupt order. This is
    // the case the power-of-two mask exists to make cheap, so it is the case
    // most worth proving.
    {
        LockFreeQueue<int, 4> q;
        for (int i = 0; i < 1000; ++i) {
            CHECK(q.push(i));
            int out = -1;
            CHECK(q.pop(out));
            CHECK(out == i);
            CHECK(q.empty());
        }
    }

    // Interleaved partial drain across the wrap point.
    {
        LockFreeQueue<int, 8> q;
        int next_push = 0, next_pop = 0;
        for (int round = 0; round < 100; ++round) {
            for (int i = 0; i < 5; ++i) CHECK(q.push(next_push++));
            for (int i = 0; i < 5; ++i) {
                int out = -1;
                CHECK(q.pop(out));
                CHECK(out == next_pop++);
            }
        }
        CHECK(q.empty());
    }

    // A queue big enough to matter, exercised at its exact boundary.
    {
        auto q = std::make_unique<LockFreeQueue<uint32_t, 1024>>();
        for (uint32_t i = 0; i < 1023; ++i) CHECK(q->push(i));
        CHECK(!q->push(9999));
        uint32_t out = 0;
        for (uint32_t i = 0; i < 1023; ++i) {
            CHECK(q->pop(out));
            CHECK(out == i);
        }
        CHECK(q->empty());
        CHECK(!q->pop(out));
    }
}
