#pragma once
#include <array>
#include <vector>
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
#include "matching/engine.h"   // TscTuple, used in the queue members below

#include "core/timer.h"

struct OrderLogEntry {
    uint64_t timestamp_tsc;
    DropCopyMessage msg;
};

// Fixed 64-byte header at the start of order_audit.log. write_index is published
// after every appended entry, so an external reader (e.g. the Python monitor) can
// tail the log while the engine runs, and it survives a crash — unlike the old
// scheme where the valid count was only written to the file size on clean shutdown.
constexpr uint64_t AUDIT_LOG_MAGIC = 0x48465441554401ULL; // "HFTAUD" + version byte
struct alignas(64) AuditLogHeader {
    uint64_t magic;                    // AUDIT_LOG_MAGIC
    uint32_t version;                  // format version
    uint32_t entry_size;              // sizeof(OrderLogEntry), for reader self-check
    std::atomic<uint64_t> write_index; // count of valid entries committed so far
};
static_assert(sizeof(AuditLogHeader) == 64, "AuditLogHeader must be exactly one cache line");
static_assert(alignof(OrderLogEntry) <= 64 && (64 % alignof(OrderLogEntry)) == 0,
              "entries must stay aligned when placed right after the header");

template<int NUM_SHARDS>
class OrderManager {
private:
    std::array<std::unique_ptr<LockFreeQueue<DropCopyMessage, 1048576>>, NUM_SHARDS>& drop_copy_queues;
    std::vector<std::unique_ptr<LockFreeQueue<DropCopyMessage, 1048576>>>& gw_reject_queues;
    std::array<std::unique_ptr<LockFreeQueue<TscTuple, 1048576>>, NUM_SHARDS>& tsc_queues;
    Timer& timer;
    std::atomic<bool>& running;
    
    int fd;
    void* mmap_base = MAP_FAILED;
    AuditLogHeader* header = nullptr;
    OrderLogEntry* mmap_entries = nullptr;
    size_t log_index = 0;
    const size_t MAX_LOG_ENTRIES = 20000000; // 20 million events capacity

    uint64_t new_orders = 0;
    uint64_t trades = 0;
    uint64_t rejects = 0;

public:
    OrderManager(std::array<std::unique_ptr<LockFreeQueue<DropCopyMessage, 1048576>>, NUM_SHARDS>& dcq,
                 std::vector<std::unique_ptr<LockFreeQueue<DropCopyMessage, 1048576>>>& reject_qs,
                 std::array<std::unique_ptr<LockFreeQueue<TscTuple, 1048576>>, NUM_SHARDS>& tq,
                 Timer& tmr,
                 std::atomic<bool>& r)
        : drop_copy_queues(dcq), gw_reject_queues(reject_qs), tsc_queues(tq), timer(tmr), running(r) {
        
        fd = open("order_audit.log", O_RDWR | O_CREAT | O_TRUNC, 0666);
        if (fd == -1) {
            throw std::runtime_error("Failed to open order audit log");
        }

        size_t total = sizeof(AuditLogHeader) + MAX_LOG_ENTRIES * sizeof(OrderLogEntry);
        if (ftruncate(fd, total) != 0) {
            close(fd);
            throw std::runtime_error("Failed to set size for order audit log");
        }

        mmap_base = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (mmap_base == MAP_FAILED) {
            close(fd);
            throw std::runtime_error("Failed to mmap order audit log");
        }
        header = reinterpret_cast<AuditLogHeader*>(mmap_base);
        mmap_entries = reinterpret_cast<OrderLogEntry*>(
            reinterpret_cast<char*>(mmap_base) + sizeof(AuditLogHeader));

        header->magic = AUDIT_LOG_MAGIC;
        header->version = 1;
        header->entry_size = sizeof(OrderLogEntry);
        header->write_index.store(0, std::memory_order_release);
    }

    ~OrderManager() {
        size_t total = sizeof(AuditLogHeader) + MAX_LOG_ENTRIES * sizeof(OrderLogEntry);
        if (mmap_base != MAP_FAILED) {
            munmap(mmap_base, total);
        }
        if (fd != -1) {
            // Trim to header + the entries actually written (readers use write_index).
            // Best-effort: a failed trim leaves trailing zeroed entries, which readers
            // already ignore because they bound their scan by write_index.
            if (ftruncate(fd, sizeof(AuditLogHeader) + log_index * sizeof(OrderLogEntry)) != 0) {
                perror("ftruncate(order_audit.log)");
            }
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
            
            // 1. Drain Drop Copy Queues (engine-produced: NEW / FILL / CANCEL)
            //    and the per-worker gateway reject queues (pre-trade REJECTED).
            //    Each queue has exactly one producer, preserving the SPSC contract.
            auto consume = [&](DropCopyMessage& m) {
                found = true;
                if (log_index < MAX_LOG_ENTRIES) {
                    mmap_entries[log_index].timestamp_tsc = __builtin_ia32_rdtsc();
                    mmap_entries[log_index].msg = m;
                    log_index++;
                    // Publish the entry (release) so a reader that acquire-loads
                    // write_index sees fully-written entry data below it.
                    header->write_index.store(log_index, std::memory_order_release);
                }

                if (m.state == OrderState::NEW) new_orders++;
                else if (m.state == OrderState::FILLED || m.state == OrderState::PARTIAL_FILL) trades++;
                else if (m.state == OrderState::REJECTED) rejects++;
            };

            for (int i = 0; i < NUM_SHARDS; ++i) {
                while (drop_copy_queues[i]->pop(msg)) consume(msg);
            }
            for (auto& rq : gw_reject_queues) {
                while (rq->pop(msg)) consume(msg);
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
