#include "tests.h"
#include "matching/order.h"
#include "core/memory_pool.h"
#include <vector>
#include <unordered_set>
#include <mutex>
#include <atomic>
#include <queue>
#include <condition_variable>

// Phase 2.1: exhaustion is a client behaviour, not a process fault. allocate()
// must hand back nullptr when the pool is full (never throw / crash), and the
// pool must keep working after a slot is freed.
void test_pool() {
    // Heap-allocated: MemoryPool embeds a 64MB recycle_queue, too big for the
    // stack. capacity 3, but slot 0 is the reserved null handle -> 2 usable.
    auto pool = std::make_unique<MemoryPool<Order>>(3, 1);

    Order* a = pool->allocate(0, 0, 1ull, 1u, 100ull, 10u, uint16_t{0}, Side::BUY);
    Order* b = pool->allocate(0, 0, 2ull, 2u, 100ull, 10u, uint16_t{0}, Side::BUY);
    CHECK(a != nullptr);
    CHECK(b != nullptr);

    // Full now: the next allocate returns null instead of throwing.
    Order* c = pool->allocate(0, 0, 3ull, 3u, 100ull, 10u, uint16_t{0}, Side::BUY);
    CHECK(c == nullptr);
    // The failed allocate must not leak the high-water mark.
    CHECK(pool->slots_used() == 3);

    // Free one slot; the pool hands it straight back and stays usable.
    pool->deallocate(a);
    Order* d = pool->allocate(0, 0, 4ull, 4u, 100ull, 10u, uint16_t{0}, Side::BUY);
    CHECK(d != nullptr);
}

void test_pool_concurrency() {
    auto pool = std::make_unique<MemoryPool<Order>>(4000, 4);

    std::vector<std::thread> alloc_threads;
    std::mutex mtx;
    std::unordered_set<uint32_t> all_indices;
    
    for (int w = 0; w < 4; ++w) {
        alloc_threads.emplace_back([&pool, w, &mtx, &all_indices]() {
            std::vector<uint32_t> worker_indices;
            while (true) {
                Order* o = pool->allocate(w, 0, 1ull, 1u, 100ull, 10u, uint16_t{0}, Side::BUY);
                if (!o) break; 

                uint32_t index = pool->index_of(o);
                
                CHECK(index != 0);
                
                uint32_t slice_capacity = pool->capacity() / 4;
                uint32_t range_start = (w * slice_capacity) + (w == 0 ? 1 : 0);
                uint32_t range_end = (w + 1) * slice_capacity;
                CHECK(index >= range_start && index < range_end);
                
                worker_indices.push_back(index);
            }
            
            uint32_t expected_slots = (w == 0) ? 999 : 1000;
            CHECK(worker_indices.size() == expected_slots);

            std::lock_guard<std::mutex> lock(mtx);
            for (uint32_t idx : worker_indices) {
                CHECK(all_indices.insert(idx).second == true); 
            }
        });
    }

    for (auto& t : alloc_threads) {
        t.join();
    }

    auto pool2 = std::make_unique<MemoryPool<Order>>(100, 4); 
    Order* o1 = pool2->allocate(1, 0, 1ull, 1u, 100ull, 10u, uint16_t{0}, Side::BUY);
    uint32_t idx1 = pool2->index_of(o1);

    std::thread dealloc_thread([&pool2, o1]() {
        pool2->deallocate(o1);
    });
    dealloc_thread.join();

    Order* o2 = pool2->allocate(1, 0, 1ull, 1u, 100ull, 10u, uint16_t{0}, Side::BUY);
    uint32_t idx2 = pool2->index_of(o2);

    CHECK(idx1 == idx2); 
}

void test_pool_recycle_concurrency() {
    auto pool = std::make_unique<MemoryPool<Order>>(1024, 1);
    auto in_use = std::make_unique<std::atomic<bool>[]>(1024);
    for (int i = 0; i < 1024; ++i) in_use[i].store(false, std::memory_order_relaxed);

    std::queue<Order*> handover;
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;

    std::thread consumer([&]() {
        while (true) {
            Order* o = nullptr;
            {
                std::unique_lock<std::mutex> lock(mtx);
                cv.wait(lock, [&]() { return !handover.empty() || done; });
                if (handover.empty() && done) break;
                o = handover.front();
                handover.pop();
            }
            if (o) {
                uint32_t index = pool->index_of(o);
                bool was_in_use = in_use[index].exchange(false, std::memory_order_relaxed);
                CHECK(was_in_use == true);
                pool->deallocate(o);
            }
        }
    });

    for (int i = 0; i < 300000; ++i) {
        Order* o = pool->allocate(0, 0, 1ull, 1u, 100ull, 10u, uint16_t{0}, Side::BUY);
        while (!o) { 
            std::this_thread::yield();
            o = pool->allocate(0, 0, 1ull, 1u, 100ull, 10u, uint16_t{0}, Side::BUY);
        }
        
        uint32_t index = pool->index_of(o);
        bool was_in_use = in_use[index].exchange(true, std::memory_order_relaxed);
        CHECK(was_in_use == false);

        {
            std::lock_guard<std::mutex> lock(mtx);
            handover.push(o);
        }
        cv.notify_one();
    }

    {
        std::lock_guard<std::mutex> lock(mtx);
        done = true;
    }
    cv.notify_all();
    consumer.join();
}
