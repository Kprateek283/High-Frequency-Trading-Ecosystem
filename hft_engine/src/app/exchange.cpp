#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <iostream>
#include <thread>
#include <atomic>
#include <cstdlib>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <vector>
#include <netinet/tcp.h>
#include <csignal>
#include <cstdio>
#include "matching/engine.h"
#include "core/timer.h"
#include "core/stats_region.h"
#include "core/memory_pool.h"
#include "core/lock_free_queue.h"
#include "gateway/tcp_server.h"
#include "market_data/publisher.h"
#include "auxiliary/order_manager.h"

EngineStats g_stats;
const int TOTAL_ORDERS_EXPECTED = 1000000;
std::atomic<bool> g_running{true};

// An order's internal_id is its pool slot index, and SessionManager packs that
// id into a fixed-width field alongside the instrument and the owning client.
// If the pool ever outgrows that field, ids would silently wrap into the
// neighbouring bits and cancels would resolve to the wrong order.
static_assert(POOL_CAPACITY_PER_SHARD <= SessionManager::MAX_INTERNAL_ID,
              "pool capacity exceeds the session map's internal_id field");



void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        g_running.store(false);
    }
}

void set_realtime_priority(int core_id) {
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        std::cerr << "Warning: Could not lock memory." << std::endl;
    }
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

    struct sched_param param;
    param.sched_priority = 99; 
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) {
        std::cerr << "Warning: Could not set SCHED_FIFO." << std::endl;
    }
}





int main() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // 1. Instantiate Core Utilities
    constexpr int NUM_SHARDS = 4; // Restored to 4 for maximum HFT capacity

    // Number of SO_REUSEPORT gateway workers. Each worker gets its own SPSC
    // ingress queue into every shard (and its own reject queue) so that no
    // LockFreeQueue ever has more than one producer.
    const char* env_threads = std::getenv("GATEWAY_THREADS");
    int num_gw = env_threads ? std::atoi(env_threads) : 1;
    if (num_gw < 1) num_gw = 1;
    if (num_gw > 16) num_gw = 16;

    std::array<std::unique_ptr<MemoryPool<Order>>, NUM_SHARDS> pools;
    std::array<std::unique_ptr<LockFreeQueue<ItchMessage, 1048576>>, NUM_SHARDS> mkt_data_queues;
    std::array<std::unique_ptr<LockFreeQueue<DropCopyMessage, 1048576>>, NUM_SHARDS> drop_copy_queues;
    std::vector<std::unique_ptr<LockFreeQueue<DropCopyMessage, 1048576>>> gw_reject_queues; // one per gateway worker
    std::array<std::vector<std::unique_ptr<LockFreeQueue<EngineTask, 524288>>>, NUM_SHARDS> engine_queues; // [shard][worker]
    std::array<std::unique_ptr<LockFreeQueue<TscTuple, 1048576>>, NUM_SHARDS> tsc_queues;
    std::array<std::unique_ptr<Engine>, NUM_SHARDS> engines;

    for (int w = 0; w < num_gw; ++w) {
        gw_reject_queues.push_back(std::make_unique<LockFreeQueue<DropCopyMessage, 1048576>>());
    }

    for (int i = 0; i < NUM_SHARDS; ++i) {
        pools[i] = std::make_unique<MemoryPool<Order>>(POOL_CAPACITY_PER_SHARD); // must match orders_by_id size
        mkt_data_queues[i] = std::make_unique<LockFreeQueue<ItchMessage, 1048576>>();
        drop_copy_queues[i] = std::make_unique<LockFreeQueue<DropCopyMessage, 1048576>>();
        tsc_queues[i] = std::make_unique<LockFreeQueue<TscTuple, 1048576>>();
        for (int w = 0; w < num_gw; ++w) {
            engine_queues[i].push_back(std::make_unique<LockFreeQueue<EngineTask, 524288>>());
        }
        engines[i] = std::make_unique<Engine>(i, *pools[i], *mkt_data_queues[i], *drop_copy_queues[i], engine_queues[i], *tsc_queues[i], g_running);
    }

    Timer timer(TOTAL_ORDERS_EXPECTED);

    // Shared-memory stats region (Phase 4). Optional: if it can't be mapped the
    // engine runs unchanged, just without observability.
    const char* shm_path = std::getenv("STATS_SHM_PATH");
    HftStatsRegion* stats = map_stats_region(shm_path ? shm_path : "/dev/shm/hft_stats");
    if (stats) {
        init_stats_region(stats, NUM_SHARDS, timer.cycles_per_ns_value());
    } else {
        std::cerr << "Warning: could not map stats region; running without it." << std::endl;
    }

    // 2. Instantiate Business Logic Components
    Publisher mkt_publisher(mkt_data_queues, g_running);
    OrderManager<NUM_SHARDS> order_manager(drop_copy_queues, gw_reject_queues, tsc_queues, timer, g_running);
    order_manager.set_stats_region(stats);
    TCPServer server(9091, num_gw, engine_queues, gw_reject_queues, pools);

    // 3. Spawn Threads and Pin to Isolated Cores
    std::thread mkt_thread([&]() { 
        set_realtime_priority(12); // E-Core
        mkt_publisher.run(); 
    });

    std::array<std::thread, NUM_SHARDS> engine_threads;
    for (int i = 0; i < NUM_SHARDS; ++i) {
        engine_threads[i] = std::thread([&engines, i]() {
            set_realtime_priority((i * 2) + 2); // Avoid core 0 and 1, start at 2, uses 2, 4, 6, 8
            pthread_setname_np(pthread_self(), ("Engine-" + std::to_string(i)).c_str());
            engines[i]->run();
        });
    }

    std::thread server_thread([&]() {
        set_realtime_priority(10); // E-Core
        pthread_setname_np(pthread_self(), "Gateway");
        server.run(g_running);
    });

    std::thread om_thread([&]() {
        set_realtime_priority(14); // E-Core
        order_manager.run();
    });

    // Background monitor thread logic moved to MetricsAndOrderManager

    std::cout << "Gateway listening on port 9091. Press Ctrl+C to shutdown." << std::endl;

    // Liveness metadata (4.5): PID file + a single READY line the orchestrator
    // waits on. Sockets are bound (TCPServer ctor) and every thread is spawned and
    // pinning itself; a short settle lets the workers reach their pin before READY.
    const char* pid_path = std::getenv("PID_FILE");
    if (FILE* pf = std::fopen(pid_path ? pid_path : "exchange.pid", "w")) {
        std::fprintf(pf, "%d\n", getpid());
        std::fclose(pf);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::cout << "READY" << std::endl;

    // 4. Wait for shutdown; meanwhile sample the stats region every ~100ms. This
    // is the single seqlock writer of the sampled block: queue depths, pool
    // high-water, the gateway cycle attribution, and the dropped-* counters
    // mirrored out of g_stats. The per-shard order counters and heartbeat are
    // written by the OrderManager; the anchor was written once above.
    while (g_running.load()) {
        if (stats) {
            stats_write(stats, [&]() {
                for (int i = 0; i < NUM_SHARDS; ++i) {
                    uint64_t eng = 0;
                    for (auto& q : engine_queues[i]) eng += q->size();
                    stats->shards[i].engine_q_depth.store(eng, std::memory_order_relaxed);
                    stats->shards[i].dropcopy_q_depth.store(drop_copy_queues[i]->size(), std::memory_order_relaxed);
                    stats->shards[i].mktdata_q_depth.store(mkt_data_queues[i]->size(), std::memory_order_relaxed);
                    stats->shards[i].pool_high_water.store(pools[i]->slots_used(), std::memory_order_relaxed);
                }
                stats->epoll_cycles.store(server.total_epoll_cycles.load(std::memory_order_relaxed), std::memory_order_relaxed);
                stats->read_cycles.store(server.total_read_cycles.load(std::memory_order_relaxed), std::memory_order_relaxed);
                stats->decode_cycles.store(server.total_decode_cycles.load(std::memory_order_relaxed), std::memory_order_relaxed);
                stats->validation_cycles.store(server.total_validation_cycles.load(std::memory_order_relaxed), std::memory_order_relaxed);
                stats->enqueue_cycles.store(server.total_enqueue_cycles.load(std::memory_order_relaxed), std::memory_order_relaxed);
                stats->orders_processed.store(server.total_orders_processed.load(std::memory_order_relaxed), std::memory_order_relaxed);
                stats->dropped_reports.store(g_stats.dropped_reports.load(std::memory_order_relaxed), std::memory_order_relaxed);
                stats->dropped_drop_copies.store(g_stats.dropped_drop_copies.load(std::memory_order_relaxed), std::memory_order_relaxed);
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "\nShutdown signal received. Joining threads..." << std::endl;
    g_running.store(false);

    // 5. Join every producer/consumer of the shared queues BEFORE the final drain,
    // so main is the sole accessor of tsc_queues below (the OrderManager also drains
    // them, so draining here while it runs would be a multi-consumer SPSC violation
    // and a race on the Timer).
    if (mkt_thread.joinable()) mkt_thread.join();
    for (int i = 0; i < NUM_SHARDS; ++i) {
        if (engine_threads[i].joinable()) engine_threads[i].join();
    }
    if (server_thread.joinable()) server_thread.join();
    if (om_thread.joinable()) om_thread.join();

    // 6. Drain any latency samples the OrderManager didn't consume, then report.
    TscTuple t;
    for (int i = 0; i < NUM_SHARDS; ++i) {
        while (tsc_queues[i]->pop(t)) {
            timer.add_latency(t.egress - t.ingress);
        }
    }

    timer.printStats("Final Session Stats");
    server.print_experiment_4_stats();
    std::cout << "\nMarket-data reports dropped (ITCH queue full): "
              << g_stats.dropped_reports.load(std::memory_order_relaxed) << std::endl;
    std::cout << "Drop-copies dropped (drop-copy queue full): "
              << g_stats.dropped_drop_copies.load(std::memory_order_relaxed) << std::endl;
    std::cout << "Engine Halted." << std::endl;

    return 0;
}
