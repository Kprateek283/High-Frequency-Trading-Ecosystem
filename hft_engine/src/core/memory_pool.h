#pragma once
#include <cstdint>
#include <new>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <sys/mman.h>
#include <iostream>
#include <memory>
#include <vector>
#include "core/lock_free_queue.h"

// Define MAP_HUGETLB if not present (older headers)
#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0x40000
#endif

template<typename T>
class MemoryPool {
private:
    T* pool;
    std::vector<std::unique_ptr<LockFreeQueue<uint32_t, 524288>>> recycle_queues;

    uint32_t pool_capacity;
    uint32_t slice_capacity;
    uint32_t num_workers;

    struct alignas(64) WorkerState {
        std::atomic<uint32_t> high_water_mark{0};
    };
    std::unique_ptr<WorkerState[]> states;

public:
    explicit MemoryPool(uint32_t capacity, uint32_t workers = 1)
        : pool_capacity(capacity), num_workers(workers) {
        
        slice_capacity = capacity / num_workers;
        states = std::make_unique<WorkerState[]>(num_workers);
        for (uint32_t i = 0; i < num_workers; ++i) {
            states[i].high_water_mark.store((i * slice_capacity) + (i == 0 ? 1 : 0), std::memory_order_relaxed);
            recycle_queues.push_back(std::make_unique<LockFreeQueue<uint32_t, 524288>>());
        }

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
    T* allocate(int worker_id, Args&&... args) {
        uint32_t index;
        if (recycle_queues[worker_id]->pop(index)) {
            // Reused!
        } else {
            uint32_t next = states[worker_id].high_water_mark.load(std::memory_order_relaxed);
            if (next < (worker_id + 1) * slice_capacity) [[likely]] {
                states[worker_id].high_water_mark.store(next + 1, std::memory_order_relaxed);
                index = next;
            } else {
                return nullptr;
            }
        }
        return new (&pool[index]) T(std::forward<Args>(args)...);
    }

    void deallocate(T* ptr) {
        uint32_t index = static_cast<uint32_t>(ptr - pool);
        int worker_id = index / slice_capacity;
        while (!recycle_queues[worker_id]->push(index)) [[unlikely]] {
            __builtin_ia32_pause();
        }
    }

    inline uint32_t index_of(const T* ptr) const {
        return static_cast<uint32_t>(ptr - pool);
    }

    inline uint32_t slots_used() const { 
        uint32_t total = 0;
        for (uint32_t i = 0; i < num_workers; ++i) {
            total += (states[i].high_water_mark.load(std::memory_order_relaxed) - (i * slice_capacity));
        }
        return total; 
    }
    inline uint32_t capacity() const { return pool_capacity; }
};
