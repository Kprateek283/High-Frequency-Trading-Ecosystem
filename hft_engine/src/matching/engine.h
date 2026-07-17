#pragma once
#include <array>
#include <atomic>
#include <vector>
#include <memory>
#include "matching/orderbook.h"
#include "core/lock_free_queue.h"

const uint16_t MAX_INSTRUMENTS = 256;

struct TscTuple {
    uint64_t ingress;
    uint64_t egress;
};

class Engine {
public:
    // Each gateway worker owns its own SPSC ingress queue into this shard; the
    // engine (single consumer) fans in from all of them, preserving the SPSC
    // invariant of LockFreeQueue under a multi-threaded gateway.
    Engine(uint16_t shard_id, MemoryPool<Order>& pool,
           LockFreeQueue<ItchMessage, 1048576>& mkt_data_queue,
           LockFreeQueue<DropCopyMessage, 1048576>& drop_copy_queue,
           std::vector<std::unique_ptr<LockFreeQueue<EngineTask, 524288>>>& engine_queues_for_shard,
           LockFreeQueue<TscTuple, 1048576>& tsc_queue,
           std::atomic<bool>& running_flag);

    void run();

private:
    std::array<Order*, MAX_ORDERS_LOOKUP> orders_by_id;
    std::array<OrderBook, MAX_INSTRUMENTS> books;
    std::vector<LockFreeQueue<EngineTask, 524288>*> queues; // one per gateway worker
    LockFreeQueue<TscTuple, 1048576>& tsc_queue;
    std::atomic<bool>& running;
};
