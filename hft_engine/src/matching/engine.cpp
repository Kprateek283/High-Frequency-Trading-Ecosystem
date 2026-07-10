#include "matching/engine.h"
#include "core/timer.h"
#include <iostream>
#include <pthread.h>

// Removed atomic global counter to eliminate cache bouncing

Engine::Engine(uint16_t shard_id, MemoryPool<Order>& pool, LockFreeQueue<ItchMessage, 1048576>& mkt_data_queue,
               LockFreeQueue<DropCopyMessage, 1048576>& drop_copy_queue,
               std::vector<std::unique_ptr<LockFreeQueue<EngineTask, 524288>>>& engine_queues_for_shard,
               LockFreeQueue<TscTuple, 1048576>& tsc_q, std::atomic<bool>& running_flag)
    : tsc_queue(tsc_q), running(running_flag), total_processed(0) {

    orders_by_id.fill(nullptr);

    queues.reserve(engine_queues_for_shard.size());
    for (auto& q : engine_queues_for_shard) {
        queues.push_back(q.get());
    }

    for (uint16_t i = shard_id; i < MAX_INSTRUMENTS; i += 4) {
        books[i].init(&pool, &mkt_data_queue, &drop_copy_queue, orders_by_id.data());
    }
}

void Engine::run() {
    pthread_setname_np(pthread_self(), "MatchingEngine");
    EngineTask task;

    while (running.load(std::memory_order_relaxed)) {
        bool found = false;

        // Fan in from every gateway worker's SPSC queue for this shard.
        for (auto* queue : queues) {
            if (!queue->pop(task)) continue;
            found = true;

            if (task.type == MsgType::NEW) {
                uint16_t inst = task.order->instrument_id;
                if (inst < MAX_INSTRUMENTS) [[likely]] {
                    books[inst].match_order(task.order);
                } else {
                    // Reject or ignore invalid instrument
                }
            } else {
                // Cancel. internal_id is a pool slot that recycles, so verify the
                // slot's current occupant is still the order this cancel refers to
                // (client_order_id); otherwise it's a stale cancel for an order that
                // already left the book and the slot now holds someone else's order.
                uint64_t idx = task.cancel.internal_id;
                Order* o = orders_by_id[idx];
                if (o && o->client_order_id == task.cancel.client_order_id) [[likely]] {
                    books[o->instrument_id].cancel_order(idx);
                }
            }

            TscTuple t = {task.ingress_tsc, get_tsc()};
            while (!tsc_queue.push(t)) { __builtin_ia32_pause(); }

            total_processed++;
        }

        if (!found) {
            __builtin_ia32_pause();
        }
    }
}
