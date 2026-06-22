#pragma once
#include <array>
#include <thread>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <iostream>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include "core/lock_free_queue.h"
#include "matching/order.h"

#include "core/timer.h"

struct OrderLogEntry {
    uint64_t timestamp_tsc;
    DropCopyMessage msg;
};

template<int NUM_SHARDS>
class OrderManager {
private:
    std::array<std::unique_ptr<LockFreeQueue<DropCopyMessage, 1048576>>, NUM_SHARDS>& drop_copy_queues;
    std::array<std::unique_ptr<LockFreeQueue<TscTuple, 1048576>>, NUM_SHARDS>& tsc_queues;
    Timer& timer;
    std::atomic<bool>& running;
    
    int fd;
    OrderLogEntry* mmap_log;
    size_t log_index = 0;
    const size_t MAX_LOG_ENTRIES = 20000000; // 20 million events capacity

    uint64_t new_orders = 0;
    uint64_t trades = 0;
    uint64_t rejects = 0;

public:
    OrderManager(std::array<std::unique_ptr<LockFreeQueue<DropCopyMessage, 1048576>>, NUM_SHARDS>& dcq, 
                 std::array<std::unique_ptr<LockFreeQueue<TscTuple, 1048576>>, NUM_SHARDS>& tq,
                 Timer& tmr,
                 std::atomic<bool>& r) 
        : drop_copy_queues(dcq), tsc_queues(tq), timer(tmr), running(r) {
        
        fd = open("order_audit.log", O_RDWR | O_CREAT | O_TRUNC, 0666);
        if (fd == -1) {
            throw std::runtime_error("Failed to open order audit log");
        }
        
        size_t size = MAX_LOG_ENTRIES * sizeof(OrderLogEntry);
        if (ftruncate(fd, size) != 0) {
            close(fd);
            throw std::runtime_error("Failed to set size for order audit log");
        }
        
        mmap_log = (OrderLogEntry*)mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (mmap_log == MAP_FAILED) {
            close(fd);
            throw std::runtime_error("Failed to mmap order audit log");
        }
    }
    
    ~OrderManager() {
        if (mmap_log != MAP_FAILED) {
            munmap(mmap_log, MAX_LOG_ENTRIES * sizeof(OrderLogEntry));
        }
        if (fd != -1) {
            ftruncate(fd, log_index * sizeof(OrderLogEntry));
            close(fd);
        }
    }
    
    void run() {
        pthread_setname_np(pthread_self(), "MetricsAndOrderManager");
        
        DropCopyMessage msg;
        TscTuple t;
        
        auto last_print_time = std::chrono::steady_clock::now();
        
        while (running.load(std::memory_order_relaxed)) {
            bool found = false;
            
            // 1. Drain Drop Copy Queues
            for (int i = 0; i < NUM_SHARDS; ++i) {
                while (drop_copy_queues[i]->pop(msg)) {
                    found = true;
                    if (log_index < MAX_LOG_ENTRIES) {
                        mmap_log[log_index].timestamp_tsc = __builtin_ia32_rdtsc();
                        mmap_log[log_index].msg = msg;
                        log_index++;
                    }
                    
                    if (msg.state == OrderState::NEW) new_orders++;
                    else if (msg.state == OrderState::FILLED || msg.state == OrderState::PARTIAL_FILL) trades++;
                    else if (msg.state == OrderState::REJECTED) rejects++;
                }
            }
            
            // 2. Drain TSC Queues for Latency Metrics
            for (int i = 0; i < NUM_SHARDS; ++i) {
                while (tsc_queues[i]->pop(t)) {
                    found = true;
                    timer.add_latency(t.egress - t.ingress);
                }
            }
            
            // 3. Print Real-Time Metrics every 1 second
            auto now = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_print_time).count();
            if (duration >= 1) {
                if (new_orders > 0 || trades > 0 || rejects > 0) {
                    double reject_rate = (new_orders + rejects) > 0 ? (double)rejects / (new_orders + rejects) * 100.0 : 0.0;
                    
                    std::cerr << "\n[Metrics] Real-Time 1s Window:"
                              << "\n  Orders/sec: " << new_orders
                              << "\n  Trades/sec: " << trades
                              << "\n  Reject Rate: " << reject_rate << "%" << std::endl;
                    
                    timer.printStats("Latency (Last Window)");
                    timer.clear();
                    
                    new_orders = 0;
                    trades = 0;
                    rejects = 0;
                }
                last_print_time = now;
            }

            if (!found) {
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }
        }
    }
};
