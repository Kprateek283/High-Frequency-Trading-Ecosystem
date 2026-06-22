#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <iostream>
#include <thread>
#include <atomic>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <vector>
#include <netinet/tcp.h>
#include <csignal>
#include "matching/engine.h"
#include "core/timer.h"
#include "core/memory_pool.h"
#include "core/lock_free_queue.h"
#include "gateway/tcp_server.h"
#include "market_data/publisher.h"
#include "auxiliary/order_manager.h"

EngineStats g_stats;
const int TOTAL_ORDERS_EXPECTED = 1000000;
std::atomic<bool> g_running{true};



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
    std::array<std::unique_ptr<MemoryPool<Order>>, NUM_SHARDS> pools;
    std::array<std::unique_ptr<LockFreeQueue<ItchMessage, 1048576>>, NUM_SHARDS> mkt_data_queues;
    std::array<std::unique_ptr<LockFreeQueue<DropCopyMessage, 1048576>>, NUM_SHARDS> drop_copy_queues;
    std::array<std::unique_ptr<LockFreeQueue<EngineTask, 2097152>>, NUM_SHARDS> engine_queues;
    std::array<std::unique_ptr<LockFreeQueue<TscTuple, 1048576>>, NUM_SHARDS> tsc_queues;
    std::array<std::unique_ptr<Engine>, NUM_SHARDS> engines;

    for (int i = 0; i < NUM_SHARDS; ++i) {
        pools[i] = std::make_unique<MemoryPool<Order>>(500000); // 500k per shard -> 2M total
        mkt_data_queues[i] = std::make_unique<LockFreeQueue<ItchMessage, 1048576>>();
        drop_copy_queues[i] = std::make_unique<LockFreeQueue<DropCopyMessage, 1048576>>();
        engine_queues[i] = std::make_unique<LockFreeQueue<EngineTask, 2097152>>();
        tsc_queues[i] = std::make_unique<LockFreeQueue<TscTuple, 1048576>>();
        engines[i] = std::make_unique<Engine>(i, *pools[i], *mkt_data_queues[i], *drop_copy_queues[i], *engine_queues[i], *tsc_queues[i], g_running);
    }
    
    Timer timer(TOTAL_ORDERS_EXPECTED);

    // 2. Instantiate Business Logic Components
    Publisher mkt_publisher(mkt_data_queues, g_running);
    OrderManager<NUM_SHARDS> order_manager(drop_copy_queues, tsc_queues, timer, g_running);
    TCPServer server(9091, engine_queues, drop_copy_queues, pools);

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

    try {
        std::cout << "Gateway listening on port 9091. Press Ctrl+C to shutdown." << std::endl;

        // 4. Wait for shutdown signal
        while (g_running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        std::cout << "\nShutdown signal received. Capturing final stats..." << std::endl;
        
        TscTuple t;
        for (int i = 0; i < NUM_SHARDS; ++i) {
            while (tsc_queues[i]->pop(t)) {
                timer.add_latency(t.egress - t.ingress);
            }
        }
        
        timer.printStats("Final Session Stats");
        server.print_experiment_4_stats();
        std::cout << "Engine Halted." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1)); 
    } catch (const std::exception& e) {
        std::cerr << "Server Error: " << e.what() << std::endl;
    }

    // 5. Join threads
    g_running.store(false);
    if (mkt_thread.joinable()) mkt_thread.join();
    for (int i = 0; i < NUM_SHARDS; ++i) {
        if (engine_threads[i].joinable()) engine_threads[i].join();
    }
    if (server_thread.joinable()) server_thread.join();
    if (om_thread.joinable()) om_thread.join();

    return 0;
}
