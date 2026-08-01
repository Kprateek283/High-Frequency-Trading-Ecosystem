#pragma once

// SCHED_FIFO acquisition for hot-path threads, and — more importantly — an
// honest record of whether it actually worked.
//
// The failure mode this exists to prevent: pthread_setschedparam fails silently
// when RLIMIT_RTPRIO is 0, the old code warned on stderr, and the benchmark
// harness sends stderr to /dev/null. A run could therefore be published as
// "SCHED_FIFO" on the strength of `ulimit -r` in the *harness* process while the
// exchange's threads all ran SCHED_OTHER. Counters here are printed on stdout,
// which the harness captures, so the claim is checkable per run.

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ostream>
#include <pthread.h>
#include <sched.h>
#include <thread>

namespace rt {

// Priority for hot-path threads. The previous hardcoded 99 shares a band with
// kernel migration and watchdog threads; 80 leaves them headroom and still sits
// above everything in userspace. 0 disables the request entirely.
inline int priority() {
    const char* e = std::getenv("RT_PRIORITY");
    int p = e ? std::atoi(e) : 80;
    return (p < 0 || p > 99) ? 0 : p;
}

inline std::atomic<int>& requested() { static std::atomic<int> n{0}; return n; }
inline std::atomic<int>& granted()   { static std::atomic<int> n{0}; return n; }

// Raise the calling thread to SCHED_FIFO. Returns true only if the kernel
// actually granted it. Best-effort by design: a developer box without
// CAP_SYS_NICE should still run, just not claim realtime scheduling.
inline bool acquire() {
    const int prio = priority();
    if (prio == 0) return false;
    requested().fetch_add(1, std::memory_order_relaxed);
    sched_param param{};
    param.sched_priority = prio;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) return false;
    granted().fetch_add(1, std::memory_order_relaxed);
    return true;
}

// Threads call acquire() from their own start-up path, so a report printed at a
// fixed delay after spawn races them: the same build printed granted=5/5, 6/6 and
// 7/7 across otherwise identical runs, because the denominator was however many
// threads happened to have arrived. Wait for the known thread count instead.
inline void await(int expected, int timeout_ms = 2000) {
    if (priority() == 0) return;            // nothing will ever be requested
    for (int i = 0; i < timeout_ms && requested().load() < expected; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

// One machine-readable line for the harness to record with the results.
// RT_SCHED: granted=4/9 priority=80
inline void report(std::ostream& os) {
    os << "RT_SCHED: granted=" << granted().load() << "/" << requested().load()
       << " priority=" << priority() << "\n";
}

}   // namespace rt
