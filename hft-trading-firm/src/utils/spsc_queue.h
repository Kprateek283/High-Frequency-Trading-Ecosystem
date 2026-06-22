#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

template <typename T, size_t N>
class SPSCQueue {
    static_assert((N & (N - 1)) == 0, "Queue size must be a power of 2");

private:
    alignas(64) std::atomic<uint64_t> tail{0}; // Written by producer
    alignas(64) std::atomic<uint64_t> head{0}; // Written by consumer
    
    T entries[N];

public:
    inline bool enqueue(const T& item) {
        uint64_t current_tail = tail.load(std::memory_order_relaxed);
        uint64_t current_head = head.load(std::memory_order_acquire);
        
        if (current_tail - current_head >= N) {
            return false; // Queue full
        }
        
        entries[current_tail & (N - 1)] = item;
        tail.store(current_tail + 1, std::memory_order_release);
        return true;
    }

    inline bool dequeue(T& item) {
        uint64_t current_head = head.load(std::memory_order_relaxed);
        uint64_t current_tail = tail.load(std::memory_order_acquire);
        
        if (current_head == current_tail) {
            return false; // Queue empty
        }
        
        item = entries[current_head & (N - 1)];
        head.store(current_head + 1, std::memory_order_release);
        return true;
    }

    inline size_t size() const {
        uint64_t current_tail = tail.load(std::memory_order_relaxed);
        uint64_t current_head = head.load(std::memory_order_relaxed);
        return current_tail > current_head ? current_tail - current_head : 0;
    }
};
