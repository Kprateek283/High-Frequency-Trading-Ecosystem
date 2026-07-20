#include "tests.h"
#include "matching/order.h"
#include "core/memory_pool.h"
#include <memory>

// Phase 2.1: exhaustion is a client behaviour, not a process fault. allocate()
// must hand back nullptr when the pool is full (never throw / crash), and the
// pool must keep working after a slot is freed.
void test_pool() {
    // Heap-allocated: MemoryPool embeds a 64MB recycle_queue, too big for the
    // stack. capacity 3, but slot 0 is the reserved null handle -> 2 usable.
    auto pool = std::make_unique<MemoryPool<Order>>(3);

    Order* a = pool->allocate(0, 1ull, 1u, 100ull, 10u, uint16_t{0}, Side::BUY);
    Order* b = pool->allocate(0, 2ull, 2u, 100ull, 10u, uint16_t{0}, Side::BUY);
    CHECK(a != nullptr);
    CHECK(b != nullptr);

    // Full now: the next allocate returns null instead of throwing.
    Order* c = pool->allocate(0, 3ull, 3u, 100ull, 10u, uint16_t{0}, Side::BUY);
    CHECK(c == nullptr);
    // The failed allocate must not leak the high-water mark.
    CHECK(pool->slots_used() == 3);

    // Free one slot; the pool hands it straight back and stays usable.
    pool->deallocate(a);
    Order* d = pool->allocate(0, 4ull, 4u, 100ull, 10u, uint16_t{0}, Side::BUY);
    CHECK(d != nullptr);
}
