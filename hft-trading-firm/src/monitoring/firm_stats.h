#pragma once
// Firm-side seqlock stats region (firm-monitoring-plan). Symmetric to the
// exchange's core/stats_region.h: the firm mmaps this into /dev/shm and ONE
// writer -- the market-data callback thread in main.cpp -- mirrors the firm's
// live state into it every ~100ms under a seqlock, so a Python reader gets a
// consistent snapshot without touching the firm's hot path.
//
// Layout is a contract with monitoring/feeds/firm_stats_reader.py; the offsets
// there are locked to the static_asserts below. Little-endian x86-64.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "protocol/messages.h"   // PROTOCOL_VERSION, MAX_INSTRUMENTS

inline constexpr uint32_t FIRM_STATS_MAGIC = 0x4649524d;              // "FIRM"
inline constexpr int FIRM_STATS_MAX_INSTRUMENTS = MAX_INSTRUMENTS;    // 256

// Plain (non-atomic) sampled block: the single writer publishes it inside the
// seqlock, and the reader validates via the even/odd seq, so no per-field
// atomics are needed (mirrors HftStatsRegion's sampled fields).
struct alignas(64) FirmStatsRegion {
    uint32_t magic;                  // FIRM_STATS_MAGIC (published last)
    uint32_t protocol_version;       // PROTOCOL_VERSION
    std::atomic<uint32_t> seq;       // seqlock: even = stable, odd = writer in progress
    uint32_t kill_switch;            // 0/1
    uint64_t heartbeat_tsc;          // writer liveness (rdtsc)
    int64_t  realized_pnl;
    uint64_t orders_sent;
    uint64_t orders_acked;
    uint64_t in_flight;
    uint64_t ticks;
    // Per-stage cycle attribution (cumulative; divide by ticks for per-tick avg).
    uint64_t book_cycles;
    uint64_t signal_cycles;
    uint64_t strategy_cycles;
    uint64_t risk_cycles;
    uint64_t exec_cycles;
    // Connector cycle attribution (cumulative).
    uint64_t serialize_cycles;
    uint64_t send_cycles;
    uint64_t enqueue_cycles;
    uint64_t dequeue_cycles;
    int64_t  position[FIRM_STATS_MAX_INSTRUMENTS];
};

// Offsets the Python reader depends on. If any of these fire, update
// monitoring/feeds/firm_stats_reader.py in the same commit.
static_assert(offsetof(FirmStatsRegion, seq) == 8, "seq offset");
static_assert(offsetof(FirmStatsRegion, kill_switch) == 12, "kill_switch offset");
static_assert(offsetof(FirmStatsRegion, heartbeat_tsc) == 16, "heartbeat offset");
static_assert(offsetof(FirmStatsRegion, realized_pnl) == 24, "scalars offset");
static_assert(offsetof(FirmStatsRegion, position) == 136, "position offset");
static_assert(sizeof(FirmStatsRegion) == 2240, "region size (reader mmaps this)");

// Map (create + truncate) the region at `path`. Returns nullptr on failure; the
// firm then runs without monitoring rather than aborting.
inline FirmStatsRegion* map_firm_stats_region(const char* path) {
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) return nullptr;
    if (ftruncate(fd, sizeof(FirmStatsRegion)) != 0) { close(fd); return nullptr; }
    void* p = mmap(nullptr, sizeof(FirmStatsRegion), PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    close(fd);   // the mapping keeps the file alive
    if (p == MAP_FAILED) return nullptr;
    auto* r = reinterpret_cast<FirmStatsRegion*>(p);   // mmap is zero-filled
    r->protocol_version = PROTOCOL_VERSION;
    r->seq.store(0, std::memory_order_relaxed);
    r->magic = FIRM_STATS_MAGIC;   // published last: a reader that sees the magic sees the rest
    return r;
}

// Seqlock write section: bump to odd, run body, bump to even. Single writer only.
template <typename Body>
inline void firm_stats_write(FirmStatsRegion* r, Body&& body) {
    uint32_t s = r->seq.load(std::memory_order_relaxed);
    r->seq.store(s + 1, std::memory_order_release);   // odd: writer in progress
    std::atomic_thread_fence(std::memory_order_release);
    body();
    std::atomic_thread_fence(std::memory_order_release);
    r->seq.store(s + 2, std::memory_order_release);   // even: consistent
}
