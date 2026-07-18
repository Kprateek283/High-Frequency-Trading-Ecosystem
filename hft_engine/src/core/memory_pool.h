#pragma once
#include <cstdint>
#include <new>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <sys/mman.h>
#include <iostream>
#include "core/lock_free_queue.h"

// Define MAP_HUGETLB if not present (older headers)
#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0x40000
#endif

template<typename T>
class MemoryPool {
private:
    T* pool;
    LockFreeQueue<uint32_t, 16777216> recycle_queue; // 16M power-of-two

    uint32_t pool_capacity;
    uint32_t high_water_mark;

    // Serializes the allocation fast-path only. Multiple SO_REUSEPORT gateway
    // workers can allocate from the same shard pool concurrently, so the
    // high_water bump and the recycle_queue pop (an SPSC ring with a single
    // producer = the engine's deallocate) must not race across consumers.
    // The engine's deallocate() stays lock-free; only allocate() takes this.
    std::atomic_flag alloc_lock = ATOMIC_FLAG_INIT;

public:
    // NOTE: slot index 0 is reserved as the "null handle" sentinel so that an
    // internal_id / order handle of 0 unambiguously means "no order". Valid slot
    // indices therefore start at 1, hence high_water_mark starts at 1.
    explicit MemoryPool(uint32_t capacity)
        : pool_capacity(capacity), high_water_mark(1) {

        size_t size = sizeof(T) * capacity;
        
        // MAP_HUGETLB requires the size to be a multiple of the huge page size (typically 2MB)
        const size_t HUGE_PAGE_SIZE = 2 * 1024 * 1024;
        if (size % HUGE_PAGE_SIZE != 0) {
            size = ((size / HUGE_PAGE_SIZE) + 1) * HUGE_PAGE_SIZE;
        }
        
        // FIX: Hugepages (TLB Optimization)
        // Attempt to allocate using Hugepages first for better TLB hit rates.
        pool = static_cast<T*>(mmap(NULL, size, PROT_READ | PROT_WRITE, 
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0));
        
        if (pool != MAP_FAILED) {
            std::cout << "[MemoryPool] Successfully allocated " << size << " bytes using Huge Pages." << std::endl;
        } else {
            // Fallback to standard 4KB pages if hugepages aren't configured
            pool = static_cast<T*>(mmap(NULL, size, PROT_READ | PROT_WRITE, 
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
            if (pool != MAP_FAILED) {
                std::cerr << "[MemoryPool] Warning: Huge Pages failed, falling back to 4KB pages." << std::endl;
            }
        }

        if (pool == MAP_FAILED) throw std::bad_alloc();

        // Pre-fault the memory
        std::memset(static_cast<void*>(pool), 0, size);
    }

    ~MemoryPool() {
        munmap(pool, sizeof(T) * pool_capacity);
    }

    template<typename... Args>
    T* allocate(Args&&... args) {
        while (alloc_lock.test_and_set(std::memory_order_acquire)) {
            __builtin_ia32_pause();
        }
        uint32_t index;
        if (recycle_queue.pop(index)) {
            // Reused!
        } else {
            uint32_t next = high_water_mark;
            if (next < pool_capacity) [[likely]] {
                high_water_mark = next + 1;
                index = next;
            } else {
                // Exhaustion is a client behaviour (too many live orders), not a
                // process fault. Return null so the gateway can reject the order
                // exactly like a risk reject instead of tearing the engine down.
                alloc_lock.clear(std::memory_order_release);
                return nullptr;
            }
        }
        alloc_lock.clear(std::memory_order_release);
        // Construction is outside the lock: `index` is now uniquely owned by
        // this caller, so no other thread can touch pool[index].
        return new (&pool[index]) T(std::forward<Args>(args)...);
    }

    void deallocate(T* ptr) {
        uint32_t index = static_cast<uint32_t>(ptr - pool);
        while (!recycle_queue.push(index)) [[unlikely]] {
            __builtin_ia32_pause();
        }
    }

    // Slot index of a live pointer; used as the order's internal_id handle.
    inline uint32_t index_of(const T* ptr) const {
        return static_cast<uint32_t>(ptr - pool);
    }

    // Approximate number of slots ever handed out (for monitoring / headroom).
    inline uint32_t slots_used() const { return high_water_mark; }
    inline uint32_t capacity() const { return pool_capacity; }
};
