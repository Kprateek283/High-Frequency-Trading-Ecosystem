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

// started() counts threads that have reached their start-up hook AT ALL, which
// is a different question from how many asked for realtime and a different one
// again from how many got it. Keeping them separate is the whole point: the
// readiness barrier must work identically whether or not realtime is requested.
inline std::atomic<int>& started()   { static std::atomic<int> n{0}; return n; }
inline std::atomic<int>& requested() { static std::atomic<int> n{0}; return n; }
inline std::atomic<int>& granted()   { static std::atomic<int> n{0}; return n; }

// Raise the calling thread to SCHED_FIFO. Returns true only if the kernel
// actually granted it. Best-effort by design: a developer box without
// CAP_SYS_NICE should still run, just not claim realtime scheduling.
inline bool acquire() {
    // Unconditional, and before the early return. This counter is what await()
    // blocks on, so skipping it when realtime is disabled silently removes the
    // readiness barrier -- see await() below.
    started().fetch_add(1, std::memory_order_relaxed);
    const int prio = priority();
    if (prio == 0) return false;
    requested().fetch_add(1, std::memory_order_relaxed);
    sched_param param{};
    param.sched_priority = prio;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) return false;
    granted().fetch_add(1, std::memory_order_relaxed);
    return true;
}

// Block until `expected` threads have reached their start-up hook.
//
// Threads call acquire() from their own start-up path, so a report printed at a
// fixed delay after spawn races them: the same build printed granted=5/5, 6/6 and
// 7/7 across otherwise identical runs, because the denominator was however many
// threads happened to have arrived. Wait for the known thread count instead.
//
// This waits on started(), not requested(). An earlier version returned early
// when RT_PRIORITY=0 -- "nothing will ever be requested" -- which was true and
// disastrous: it made READY a 100 ms sleep in exactly the mode the benchmarks
// run in. The gateway workers were still ~2.5 s from existing when READY fired,
// so run_sharding.sh drove 20,000 orders into a socket with no reader and then
// shut the engine down, reporting 0 orders and 0 matches.
//
// The timeout is generous because thread start-up here is dominated by the
// engine preallocating its mmap'd audit log, which is slow on a cold file.
inline void await(int expected, int timeout_ms = 15000) {
    for (int i = 0; i < timeout_ms && started().load() < expected; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

// One machine-readable line for the harness to record with the results.
// RT_SCHED: granted=4/9 priority=80 threads=11
//
// threads= is the barrier's own denominator. If it is short of the expected
// count at READY, await() timed out and the process is not actually up yet --
// which is invisible from granted=/requested= alone when RT_PRIORITY=0.
inline void report(std::ostream& os) {
    os << "RT_SCHED: granted=" << granted().load() << "/" << requested().load()
       << " priority=" << priority()
       << " threads=" << started().load() << "\n";
}

}   // namespace rt
