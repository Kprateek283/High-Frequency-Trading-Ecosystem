#pragma once
#include <atomic>
#include <array>

template<typename T, size_t Capacity>
class LockFreeQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
public:
    LockFreeQueue() : head(0), cached_tail(0), tail(0), cached_head(0) {}

    // Producer (Simulating the Network Gateway)
    bool push(const T& item) {
        const size_t t = tail.load(std::memory_order_relaxed);
        
        size_t next_t = (t + 1) & (Capacity - 1);
        if (next_t == cached_head) {
            cached_head = head.load(std::memory_order_acquire);
            if (next_t == cached_head) return false; // Queue Full
        }

        buffer[t] = item;
        tail.store(next_t, std::memory_order_release);
        return true;
    }

    // Consumer (The Matching Engine)
    bool pop(T& item) {
        const size_t h = head.load(std::memory_order_relaxed);

        if (h == cached_tail) {
            cached_tail = tail.load(std::memory_order_acquire);
            if (h == cached_tail) return false; // Queue Empty
        }

        item = buffer[h];
        head.store((h + 1) & (Capacity - 1), std::memory_order_release);
        return true;
    }

    bool empty() const {
        return head.load(std::memory_order_relaxed) == tail.load(std::memory_order_relaxed);
    }

    // Current occupancy, for the stats region (Phase 4.2). Relaxed loads: a
    // monitor reads a slightly stale depth, never a torn one. Ring is power-of-two
    // so the masked difference is the live count regardless of wrap.
    size_t size() const {
        const size_t t = tail.load(std::memory_order_relaxed);
        const size_t h = head.load(std::memory_order_relaxed);
        return (t - h) & (Capacity - 1);
    }

private:
    alignas(128) std::array<T, Capacity> buffer;
    
    // Aligning to 128 bytes prevents aggressive prefetcher "False Sharing"
    alignas(128) std::atomic<size_t> head;
    alignas(128) size_t cached_tail;
    
    alignas(128) std::atomic<size_t> tail;
    alignas(128) size_t cached_head;
};
