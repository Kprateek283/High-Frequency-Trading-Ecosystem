#pragma once
// Read-only shared-memory stats/health region (prep §4 / plan 4.3, 4.4, 4.5).
//
// The engine mmaps this into /dev/shm; a monitor (Python) mmaps it read-only and
// polls it. Nothing here is on the matching hot path: owner threads fetch_add
// into their own heap atomics as they already do, and ONE coordinator thread
// (main's 100ms loop) mirrors those into the region under a seqlock so the reader
// gets a consistent snapshot. The heartbeat is written by the OrderManager loop
// so a stalled drain is detectable.
//
// Layout is a contract with Python (see monitoring/schema). It is versioned by
// PROTOCOL_VERSION; the magic and version let a reader reject a stale mapping.
// All multi-byte fields are little-endian on the target x86-64.

#include <atomic>
#include <cstdint>
#include <ctime>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "protocol/messages.h"   // PROTOCOL_VERSION
#include "core/timer.h"          // get_tsc

inline constexpr uint32_t HFT_STATS_MAGIC = 0x48465431; // "HFT1"
inline constexpr int STATS_MAX_SHARDS = 8;              // region is sized for up to 8

// TSC <-> wall-clock anchor. Python renders any wire TSC as epoch ns via
//   unix_ns_at_anchor + (tsc - tsc_at_anchor) / cycles_per_ns
// Assumes an invariant TSC (true on the target CPUs).
struct TscAnchor {
    uint64_t tsc_at_anchor;
    uint64_t unix_ns_at_anchor;
    double   cycles_per_ns;
};

// Engine-private baseline used to refine the anchor. NOT part of the shared
// layout -- adding a field to HftStatsRegion would require a matching change in
// monitoring/wire.py, and this needs no reader involvement.
struct AnchorBaseline {
    uint64_t tsc0;                    // TSC at startup
    uint64_t mono_ns0;                // CLOCK_MONOTONIC at startup
    double   startup_cycles_per_ns;   // Timer::calibrate()'s 50ms estimate
};

// Refuse to refine until the baseline is long enough to beat the 50ms startup
// sample by a wide margin.
inline constexpr uint64_t ANCHOR_MIN_BASELINE_NS = 1000000000ULL;   // 1s
// An invariant TSC does not change rate, so a refined estimate this far from the
// startup one means a broken measurement, not a real drift. Keep the last good.
inline constexpr double ANCHOR_MAX_RATE_DEVIATION = 0.20;           // +/-20%

struct alignas(64) ShardStats {
    std::atomic<uint64_t> orders_in{0};
    std::atomic<uint64_t> fills{0};
    std::atomic<uint64_t> cancels{0};
    std::atomic<uint64_t> rejects{0};
    std::atomic<uint64_t> engine_q_depth{0};
    std::atomic<uint64_t> dropcopy_q_depth{0};
    std::atomic<uint64_t> mktdata_q_depth{0};
    std::atomic<uint64_t> pool_high_water{0};
};

struct HftStatsRegion {
    uint32_t magic;                       // HFT_STATS_MAGIC
    uint32_t protocol_version;            // PROTOCOL_VERSION
    uint32_t num_shards;                  // live shard count (<= STATS_MAX_SHARDS)
    uint32_t _pad;
    std::atomic<uint32_t> seq;            // seqlock: odd = writer in progress
    TscAnchor anchor;                     // write-once at startup (4.4)
    std::atomic<uint64_t> heartbeat_tsc;  // OrderManager liveness (4.5)
    // Global counters mirrored from g_stats (engine hot path stays untouched).
    std::atomic<uint64_t> dropped_reports{0};
    std::atomic<uint64_t> dropped_drop_copies{0};
    // Gateway per-stage cycle attribution, mirrored from TCPServer.
    std::atomic<uint64_t> epoll_cycles{0};
    std::atomic<uint64_t> read_cycles{0};
    std::atomic<uint64_t> decode_cycles{0};
    std::atomic<uint64_t> validation_cycles{0};
    std::atomic<uint64_t> enqueue_cycles{0};
    std::atomic<uint64_t> orders_processed{0};
    ShardStats shards[STATS_MAX_SHARDS];
};

// Map (create + truncate) the region at `path`. Returns nullptr on failure; the
// caller runs without a stats region rather than aborting the engine.
inline HftStatsRegion* map_stats_region(const char* path) {
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) return nullptr;
    if (ftruncate(fd, sizeof(HftStatsRegion)) != 0) { close(fd); return nullptr; }
    void* p = mmap(nullptr, sizeof(HftStatsRegion), PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    close(fd);   // the mapping keeps the file alive
    if (p == MAP_FAILED) return nullptr;
    return reinterpret_cast<HftStatsRegion*>(p);   // mmap is zero-filled
}

// Fill the write-once header + the initial anchor, and record the baseline the
// refresh below refines against. Call once, before publishing READY.
inline void init_stats_region(HftStatsRegion* r, int num_shards, double cycles_per_ns,
                              AnchorBaseline* baseline) {
    struct timespec real, mono;
    clock_gettime(CLOCK_REALTIME, &real);
    clock_gettime(CLOCK_MONOTONIC, &mono);
    const uint64_t tsc = get_tsc();

    r->anchor.tsc_at_anchor = tsc;
    r->anchor.unix_ns_at_anchor =
        static_cast<uint64_t>(real.tv_sec) * 1000000000ULL + real.tv_nsec;
    r->anchor.cycles_per_ns = cycles_per_ns;

    if (baseline) {
        baseline->tsc0 = tsc;
        baseline->mono_ns0 =
            static_cast<uint64_t>(mono.tv_sec) * 1000000000ULL + mono.tv_nsec;
        baseline->startup_cycles_per_ns = cycles_per_ns;
    }

    r->num_shards = static_cast<uint32_t>(num_shards);
    r->protocol_version = PROTOCOL_VERSION;
    r->seq.store(0, std::memory_order_relaxed);
    r->magic = HFT_STATS_MAGIC;   // published last: a reader that sees the magic sees the rest
}

// Re-anchor to "now" and refine the TSC rate. Call from the seqlock writer
// thread only, inside stats_write.
//
// Timer::calibrate() estimates cycles_per_ns once from a 50ms sample. Any error
// there is a *rate* error, so converted timestamps diverge from wall clock
// without bound as the engine runs -- measured at ~48us per second of uptime,
// which put the OrderManager heartbeat visibly in the future within seconds and
// left health.assess() unable to see it (it only tests for a *stale* heartbeat).
//
// Two clocks on purpose:
//   - the rate is measured against CLOCK_MONOTONIC, which never steps, so an NTP
//     adjustment cannot corrupt it;
//   - the epoch mapping re-reads CLOCK_REALTIME every refresh, so a step there is
//     absorbed immediately instead of accumulating.
// The baseline only grows, so the rate estimate keeps improving with uptime.
inline void refresh_anchor(HftStatsRegion* r, const AnchorBaseline& base) {
    struct timespec real, mono;
    clock_gettime(CLOCK_MONOTONIC, &mono);
    const uint64_t tsc_now = get_tsc();
    clock_gettime(CLOCK_REALTIME, &real);

    const uint64_t mono_ns =
        static_cast<uint64_t>(mono.tv_sec) * 1000000000ULL + mono.tv_nsec;

    r->anchor.tsc_at_anchor = tsc_now;
    r->anchor.unix_ns_at_anchor =
        static_cast<uint64_t>(real.tv_sec) * 1000000000ULL + real.tv_nsec;

    if (mono_ns <= base.mono_ns0 || tsc_now <= base.tsc0) return;   // clock went backwards
    const uint64_t elapsed_ns = mono_ns - base.mono_ns0;
    if (elapsed_ns < ANCHOR_MIN_BASELINE_NS) return;                // too short to beat startup

    const double rate = static_cast<double>(tsc_now - base.tsc0)
                      / static_cast<double>(elapsed_ns);
    const double deviation = (rate - base.startup_cycles_per_ns) / base.startup_cycles_per_ns;
    if (deviation > -ANCHOR_MAX_RATE_DEVIATION && deviation < ANCHOR_MAX_RATE_DEVIATION) {
        r->anchor.cycles_per_ns = rate;
    }
}

// Seqlock write section: bump to odd, run body, bump to even. Single writer only.
template <typename Body>
inline void stats_write(HftStatsRegion* r, Body&& body) {
    uint32_t s = r->seq.load(std::memory_order_relaxed);
    r->seq.store(s + 1, std::memory_order_release);   // odd: writer in progress
    std::atomic_thread_fence(std::memory_order_release);
    body();
    std::atomic_thread_fence(std::memory_order_release);
    r->seq.store(s + 2, std::memory_order_release);   // even: consistent
}
