# Engineering Bottlenecks & Debugging

Building a system capable of processing 10,000,000 messages per second exposes fundamental limitations in operating systems and hardware. This document chronicles the three most significant engineering challenges encountered during development and the forensic debugging process used to solve them.

## Summary

| Bottleneck | Root Cause | Resolution |
|------------|------------|------------|
| **Multi-ms Latency Spike** | TCP receive-buffer saturation | `SO_REUSEPORT` sharding |
| **EBADF Spin Loop** | Invalid FD mapping | Correct FD ownership |
| **SIGBUS Crash** | Write past mmap'd audit log | Bound writes by `write_index` |
| **Oversubscription** | Too many active threads | 4-thread operating point |

> The cycle/millisecond figures in the war stories below are the **historical
> observations from the original debugging episodes** (pre-fix, unoptimised engine,
> 1-thread reject loop). They are not rows of the current capacity matrix — that matrix is
> `TODO(measure)` (see [`benchmarks.md`](./benchmarks.md)). Exact re-measured figures are
> pending; the diagnoses stand on their own.

---

## 1. The Multi-Millisecond Phantom Latency

### The Problem
During initial capacity testing, the system processed 1,000,000 msgs/sec with sub-millisecond latency. When pushed to a single-thread saturation load, end-to-end latency inexplicably jumped by *orders of magnitude* — from microseconds into the tens of milliseconds.

### The Investigation
The initial hypothesis was that the C++ Order Book logic (vector insertions, sorting) was scaling non-linearly and choking under load. Standard profiling tools like `gprof` and `std::chrono` lacked the granularity to prove this without introducing heavy observer overhead.

We used the hardware telemetry pipeline (the five-point `__rdtscp` decomposition — see [`telemetry.md`](./telemetry.md)) to attribute the latency to a specific stage.

### The Discovery
The telemetry disproved the hypothesis by attribution, not magnitude: **engine execution stayed flat** (a few thousand cycles regardless of load), while the entire blow-up landed in the **TCP path** (`t5 - t4_network_deq`). The C++ business logic was executing in microseconds; the Linux kernel's TCP receive buffer was saturated. The single-threaded `epoll_wait` loop could not pull bytes into userspace fast enough, so packets queued inside the OS network stack. The spike was not execution time; it was **Queueing Delay**.

> Exact cycle figures are `TODO(measure)` — the original run recorded a ~300M-cycle TCP
> path against a few-thousand-cycle engine, but on an unoptimised pre-fix engine; the
> point (attribution to the kernel path, engine flat across load) is what the re-measured
> matrix must confirm.

### The Resolution
We refactored the Exchange Gateway to use `SO_REUSEPORT`, allowing multiple independent threads to bind to the same listening port. The kernel natively load-balanced the incoming TCP streams via hashing, splitting the ingress pressure across 4 parallel `epoll` loops. This reduced the queueing delay by several orders of magnitude and restored microsecond-scale latency.

---

## 2. The `EBADF` `epoll` Spin-Loop

*(This and the `SIGBUS` in §3 were originally written up as one causal chain. They are two
independent bugs: `EBADF` is a return code and cannot itself raise `SIGBUS`. Splitting them
is the honest account.)*

### The Problem
After deploying the `SO_REUSEPORT` sharding, the Exchange Gateway threads would suddenly peg the CPU at 100% utilization and freeze all processing.

### The Investigation
`strace` revealed a massive, continuous wall of `epoll_wait` and `read` syscalls returning instantly.

### The Discovery
During the sharding refactor, an array-indexing bug passed an invalid, mathematically offset File Descriptor to `read()`.
1.  `read(invalid_fd)` returned `-1` with `errno == EBADF`.
2.  The bytes on the *valid* socket were therefore never drained from the kernel buffer.
3.  Under **Edge-Triggered (`EPOLLET`)** mode, the socket still holding unread data meant `epoll_wait` re-woke the thread immediately.
4.  The result is an inescapable tight loop of failed reads pinning the core at 100% — a **livelock**, not a crash. It burns CPU forever; it does not, by itself, produce a signal.

### The Resolution
We corrected the mapping between `epoll_event.data.fd` and our internal session state, ensuring the exact kernel fd reaches `read()`, and added an explicit `errno == EBADF` guard against silent spin-looping.

---

## 3. The `SIGBUS` on the Memory-Mapped Audit Log

### The Problem
Under sustained load the process would die with a `SIGBUS` (bus error) — a genuine memory fault, distinct from the spin-loop above.

### The Investigation
`SIGBUS` on Linux means an invalid physical access: an unaligned load/store, or a touch **past the end of an `mmap`'d region**. That pointed squarely at the one large mapping on the write path — `order_audit.log`.

### The Discovery
The `OrderManager` `mmap`s the audit log and appends fixed-size entries (`auxiliary/order_manager.h`). Writing an entry once the cursor ran past the mapped length dereferences memory beyond the mapping → `SIGBUS`.

### The Resolution
The audit log now carries a 64-byte header with an atomic `write_index`; appends are bounded by the mapped capacity and the committed count is published via `write_index` (the same field readers use to tail the log). Writes can no longer run off the end of the mapping. *(Tracked as the `[MED]` audit-log item in [`known-issues.md`](./known-issues.md), resolved.)*

---

## 4. The Thread Oversubscription Barrier

### The Problem
Having successfully achieved 10M msgs/sec with 4 Gateway threads, we attempted to scale the system further by enabling 8 Gateway threads. Paradoxically, adding more threads *increased* the end-to-end latency and degraded the stability of the Trading Engine.

### The Investigation
We observed the cycle counts in the SPSC Lock-Free Queue and the Matching Engine. At 8 threads, the cycle counts became highly erratic, exhibiting massive standard deviation spikes that suggested the threads were not running continuously.

### The Discovery
The underlying hardware was an Intel Core i5-1240P. This processor provides fewer physical execution resources than the total number of concurrently active gateway, matching, and load-generation threads.

This physically oversubscribed the CPU. The Linux OS scheduler was forced to preempt and migrate threads more frequently, increasing scheduling overhead and reducing cache locality.

### The Resolution
A 4-shard configuration is the established operating point for this machine (whether it beats 8 shards on the re-measured matrix is `TODO(measure)`). Thread affinity is **already implemented**, not future work: engines, publisher, gateway, and OrderManager are pinned via `pthread_setaffinity_np` + `SCHED_FIFO` (`app/exchange.cpp`), and Phase 3.2 extended pinning to the spawned gateway *worker* threads that previously floated (the last open item in [`known-issues.md`](./known-issues.md)). Scaling further would require a dedicated high-core-count server processor (AMD EPYC / Intel Xeon) with enough isolated cores to avoid oversubscription in the first place.
