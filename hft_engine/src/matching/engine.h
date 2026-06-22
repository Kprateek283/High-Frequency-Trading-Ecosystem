#pragma once
#include <array>
#include <atomic>
#include "matching/orderbook.h"
#include "core/lock_free_queue.h"

const uint16_t MAX_INSTRUMENTS = 256;

struct TscTuple {
    uint64_t ingress;
    uint64_t egress;
};

class Engine {
public:
    Engine(uint16_t shard_id, MemoryPool<Order>& pool, 
           LockFreeQueue<ItchMessage, 1048576>& mkt_data_queue,
           LockFreeQueue<DropCopyMessage, 1048576>& drop_copy_queue,
           LockFreeQueue<EngineTask, 2097152>& engine_queue,
           LockFreeQueue<TscTuple, 1048576>& tsc_queue,
           std::atomic<bool>& running_flag);

    void run();

    uint64_t get_total_processed() const {
        return total_processed;
    }
private:
    alignas(64) uint64_t total_processed;
    std::array<Order*, MAX_ORDERS_LOOKUP> orders_by_id;
    std::array<OrderBook, MAX_INSTRUMENTS> books;
    LockFreeQueue<EngineTask, 2097152>& queue;
    LockFreeQueue<TscTuple, 1048576>& tsc_queue;
    std::atomic<bool>& running;
};
