#include "matching/engine.h"
#include "core/timer.h"
#include <iostream>
#include <pthread.h>

// Removed atomic global counter to eliminate cache bouncing

Engine::Engine(uint16_t shard_id, MemoryPool<Order>& pool, LockFreeQueue<ItchMessage, 1048576>& mkt_data_queue,
               LockFreeQueue<DropCopyMessage, 1048576>& drop_copy_queue,
               LockFreeQueue<EngineTask, 2097152>& engine_queue, LockFreeQueue<TscTuple, 1048576>& tsc_q,
               std::atomic<bool>& running_flag)
    : queue(engine_queue), tsc_queue(tsc_q), running(running_flag), total_processed(0) {
    
    orders_by_id.fill(nullptr);
    
    for (uint16_t i = shard_id; i < MAX_INSTRUMENTS; i += 4) {
        books[i].init(&pool, &mkt_data_queue, &drop_copy_queue, orders_by_id.data());
    }
}

void Engine::run() {
    pthread_setname_np(pthread_self(), "MatchingEngine");
    EngineTask task;
    
    while (running.load(std::memory_order_relaxed)) {
        if (queue.pop(task)) {
            if (task.type == MsgType::NEW) {
                uint16_t inst = task.order->instrument_id;
                if (inst < MAX_INSTRUMENTS) [[likely]] {
                    books[inst].match_order(task.order);
                } else {
                    // Reject or ignore invalid instrument
                }
            } else {
                // Cancel
                Order* o = orders_by_id[task.internal_id];
                if (o) [[likely]] {
                    books[o->instrument_id].cancel_order(task.internal_id);
                }
            }
            
            TscTuple t = {task.ingress_tsc, get_tsc()};
            while (!tsc_queue.push(t)) { __builtin_ia32_pause(); }
            
            total_processed++;
        } else {
            __builtin_ia32_pause();
        }
    }
}
