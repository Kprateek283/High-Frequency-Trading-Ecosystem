#pragma once
#include <cstdint>
#include <stdexcept>
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

public:
    explicit MemoryPool(uint32_t capacity) 
        : pool_capacity(capacity), high_water_mark(0) {
        
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
        uint32_t index;
        if (recycle_queue.pop(index)) {
            // Reused!
        } else {
            uint32_t next = high_water_mark++;
            if (next < pool_capacity) [[likely]] {
                index = next;
            } else {
                throw std::runtime_error("Memory Pool exhausted!");
            }
        }
        return new (&pool[index]) T(std::forward<Args>(args)...);
    }

    void deallocate(T* ptr) {
        uint32_t index = static_cast<uint32_t>(ptr - pool);
        while (!recycle_queue.push(index)) [[unlikely]] {
            __builtin_ia32_pause();
        }
    }
};
