# Engineering Bottlenecks & Debugging

Building a system capable of processing 10,000,000 messages per second exposes fundamental limitations in operating systems and hardware. This document chronicles the three most significant engineering challenges encountered during development and the forensic debugging process used to solve them.

## Summary

| Bottleneck | Root Cause | Resolution |
|------------|------------|------------|
| **75ms Latency Spike** | TCP receive-buffer saturation | `SO_REUSEPORT` sharding |
| **EBADF Spin Loop** | Invalid FD mapping | Correct FD ownership |
| **Oversubscription** | Too many active threads | 4-thread operating point |

---

## 1. The 75-Millisecond Phantom Latency

### The Problem
During initial capacity testing, the system successfully processed 1,000,000 msgs/sec with sub-millisecond latency. However, when pushed to 5,000,000 msgs/sec on a single thread, the end-to-end latency inexplicably skyrocketed to ~75 milliseconds. 

### The Investigation
The initial hypothesis was that the C++ Order Book logic (vector insertions, sorting) was scaling non-linearly and choking under load. Standard profiling tools like `gprof` and `std::chrono` lacked the granularity to prove this without introducing heavy observer overhead.

We built a custom hardware telemetry pipeline using the x86 `__rdtscp` intrinsic to timestamp packets at four distinct lifecycle boundaries. 

### The Discovery
The telemetry data definitively disproved the initial hypothesis:
*   **Trading Engine Execution:** ~5,000 CPU cycles.
*   **TCP Network Path:** 299,000,000 CPU cycles (~75 milliseconds).

The C++ business logic was flawlessly executing in microseconds, but the Linux Kernel's TCP receive buffer was completely saturated. The single-threaded `epoll_wait` loop could not pull bytes into userspace fast enough, causing packets to queue inside the OS network stack. The 75ms was not execution time; it was **Queueing Delay**.

### The Resolution
We refactored the Exchange Gateway to use `SO_REUSEPORT`, allowing multiple independent threads to bind to the same listening port. The kernel natively load-balanced the incoming TCP streams via hashing, splitting the ingress pressure across 4 parallel `epoll` loops. This reduced the queueing delay by several orders of magnitude and restored microsecond-scale latency.

---

## 2. The `EBADF` `epoll` Spin-Loop Crash

### The Problem
Immediately after deploying the `SO_REUSEPORT` sharding fix, the system exhibited catastrophic failure. When blasted with traffic, the Exchange Gateway threads would suddenly peg the CPU at 100% utilization, freeze all processing, and eventually crash with a `SIGBUS` (Bus Error) memory violation.

### The Investigation
Because the crash happened in a tight event loop under massive load, standard asynchronous loggers failed to flush the error state before the `SIGBUS` killed the process. We had to rely on synchronous `cerr` dumps and `strace` to inspect the syscalls immediately preceding the crash.

### The Discovery
The `strace` output revealed a massive, continuous wall of `epoll_wait` and `read` syscalls returning instantly. 

During the sharding refactor, an array indexing bug caused the system to pass an invalid, mathematically offset File Descriptor (`local_fd`) to the `read()` syscall. 
1.  `read(invalid_fd)` returned `-1` with an `errno` of `EBADF` (Bad File Descriptor).
2.  Because the `read()` failed, the actual bytes on the *valid* socket were never drained from the kernel buffer.
3.  Because the `epoll` loop was operating in **Edge-Triggered (`EPOLLET`) mode**, and the socket still had unread data, `epoll_wait` immediately woke up the thread again on the very next nanosecond.
4.  This created an inescapable infinite loop of failed reads that drove CPU utilization to 100%, stalled useful work, and ultimately destabilized the process, culminating in a `SIGBUS` crash.

### The Resolution
We corrected the localized mapping between the `epoll_event.data.fd` and our internal session arrays, ensuring the exact kernel File Descriptor was passed to `read()`. Furthermore, we added explicit `errno == EBADF` fatal-exit guards to prevent silent spin-looping in the future.

---

## 3. The Thread Oversubscription Barrier

### The Problem
Having successfully achieved 10M msgs/sec with 4 Gateway threads, we attempted to scale the system further by enabling 8 Gateway threads. Paradoxically, adding more threads *increased* the end-to-end latency and degraded the stability of the Trading Engine.

### The Investigation
We observed the cycle counts in the SPSC Lock-Free Queue and the Matching Engine. At 8 threads, the cycle counts became highly erratic, exhibiting massive standard deviation spikes that suggested the threads were not running continuously.

### The Discovery
The underlying hardware was an Intel Core i5-1240P. This processor provides fewer physical execution resources than the total number of concurrently active gateway, matching, and load-generation threads.

This physically oversubscribed the CPU. The Linux OS scheduler was forced to preempt and migrate threads more frequently, increasing scheduling overhead and reducing cache locality.

### The Resolution
We established that for this machine, a 4-shard configuration sustained 10,000,000 msgs/sec with lower latency than the 8-shard configuration, establishing it as the optimal operating point. To scale further without degradation, the system would require a dedicated high-core-count server processor (e.g., AMD EPYC or Intel Xeon) and explicit Thread Affinity (`pthread_setaffinity_np`) to permanently pin Gateway and Engine threads to isolated NUMA cores.
