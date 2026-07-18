# Benchmarks & Capacity Planning

## Benchmark Scope
The goal of these experiments is not to measure the theoretical minimum possible latency of an isolated order in a vacuum.

Instead, the system is intentionally stressed under massive sustained load (1,000,000 to 10,000,000 messages per second) to empirically study:
- Throughput capacity limits
- Non-linear queueing effects under saturation
- Gateway horizontal scalability via `SO_REUSEPORT`
- Kernel networking overhead vs Application logic cost
- Thread oversubscription boundaries

## Test Environment

### Hardware
*   **CPU:** Intel Core i5-1240P. Cycles→time conversions use the **measured TSC
    frequency** the engine calibrates at startup (`core/timer.h`), not the 4.4 GHz
    single-core turbo ceiling from the spec sheet — sustained all-core clock on this
    part is ~4.0 GHz, and the calibrated value is the only one that matters for
    TSC-based timing.
*   **Memory Architecture:** Unified NUMA node

### Software
*   **OS:** Ubuntu 24.04.3 LTS (Linux Kernel)
*   **Compiler:** GCC / Clang, C++20. Both projects now build with the **same**
    release flags — `-O3 -march=native -flto -DNDEBUG` and `-Wall -Wextra -Werror
    -Wpedantic` (unified in Phase 0.1; the engine previously carried no flags of its
    own, so every pre-fix number was measured on an unoptimised engine).

## Measurement Methodology

### RDTSCP Timestamping
Standard timing APIs introduce measurable observer overhead relative to direct TSC reads. To achieve true cycle-accurate profiling without inflating latency (the Heisenberg effect), the system injects the x86 hardware intrinsic `__rdtscp` directly into the packet payloads. 

### Latency Attribution
Five cycle-timestamps track an order across thread and network boundaries: four ride on
the wire in `OuchEnterOrder` (`t1_exchange_send` … `t4_network_deq`) and the gateway
stamps a fifth (`t5`) when `read()` returns; the engine separately pairs a per-task
`ingress_tsc` with a match-time `get_tsc()`. See [`telemetry.md`](./telemetry.md) for the
full five-point decomposition. From these we isolate:
1.  **TCP Path:** Network traversal and kernel queueing (`t5 - t4_network_deq`).
2.  **SPSC Queue + handoff:** Lock-free inter-thread handoff.
3.  **Trading Engine:** Application business logic — Risk + Matching (`ingress_tsc → match`).

---

## Gateway Cycle Attribution

The gateway maintains per-stage cycle accumulators — `total_read_cycles`,
`total_decode_cycles`, `total_validation_cycles`, `total_enqueue_cycles` — over the live
decode path (`gateway/tcp_server.h`), exported in the shutdown stats and the `/dev/shm`
stats region. The micro-level split (`epoll_wait` / `read` / Decode / Validate / Enqueue)
and the kernel-vs-application percentage come straight from these counters.

> **`TODO(measure)`** — concrete per-stage cycle figures are **not yet re-measured** on
> reference hardware after the Phase-0 flag unification, the Phase-1 symbol fix, and the
> Phase-3 pinning/4-thread default. The previously published numbers were taken on an
> unoptimised, 1-thread, reject-loop configuration (see review A1/A2/B7/B9) and have been
> removed rather than carried forward unverified. Re-run the capacity matrix on an idle
> reference box (`scripts/run_sharding.sh`) and populate from the counters above.

---

## Capacity Scaling Matrix

The matrix sweeps load (1M–10M msgs/sec) × gateway threads (1/2/4/8), recording per row the
TCP path, engine cycles, end-to-end latency, and the accepted/rejected split.

> **`TODO(measure)`** — the full throughput/latency matrix has not been re-measured on this
> box (Phase 3.5 stop condition: the machine cannot meaningfully sustain a 10M msgs/sec
> load). Every cell is pending. The prior matrix is intentionally not reproduced here
> because it predates the symbol/decode fix that made the run actually match orders.

### What *was* verified (functional run, `results.txt`)

A 4-thread gateway run through the fixed pipeline now matches orders instead of rejecting
them — the accepted/rejected split A2 asked for:

| Metric | Value |
| :--- | ---: |
| Gateway Threads | 4 |
| Orders (NEW) | 10,000 |
| Matches (FILLED) | 20,000 |
| PARTIAL_FILL / CANCELED | 0 / 0 |
| **REJECTED** | **0** |

Reject rate is now **0%** — a property of the workload fitting the 256-instrument cap with
symbols that decode, not of a parsing failure (contrast the pre-fix 74.4% reject rate in
review A2). Throughput and latency for this run are `TODO(measure)`.

---

## Key Findings

*Numbers below are `TODO(measure)` pending the re-run; the qualitative structure is what
the design predicts and what the pre-fix runs showed directionally.*

1. **Single-Thread epoll Ceiling:** A single `epoll` ingestion loop is expected to hit a
   capacity threshold below the 10M target; pushing past it causes sustained TCP
   receive-buffer saturation and severe queueing delay.
2. **Queueing Delay Dominates Latency:** Under saturation the engine logic stays flat while
   TCP queueing delay balloons by orders of magnitude — queueing delay, not execution,
   dictates end-to-end latency.
3. **SO_REUSEPORT Scalability:** Spreading TCP ingress across shards is expected to cut
   queueing delay by orders of magnitude, restoring microsecond-scale latency.
4. **The 4-Thread Operating Point:** Four gateway threads is the configured default
   (Phase 3.3); whether it is the optimum for this machine is `TODO(measure)`.
5. **Thread Oversubscription:** Beyond the core count, 8 gateways + 8 engines + the load
   generator oversubscribe the CPU, increasing latency via cache contention and scheduler
   overhead.

## Conclusions
The functional goal — a documented run that actually matches orders through the full
pipeline — is met (`results.txt`: 4 threads, 10,000 orders, 20,000 fills, 0 rejects). The
throughput/latency campaign is `TODO(measure)` on reference hardware. The design thesis is
unchanged: the primary scaling bottleneck is expected to lie in the kernel networking path,
not the application logic, motivating future kernel-bypass work (DPDK / ef_vi).
