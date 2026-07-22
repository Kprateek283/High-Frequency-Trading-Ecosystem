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

Measured on the development box after the Phase-0/1/3 work, from the counters above:

| Stage | Cycles/order |
| :--- | ---: |
| Decode | ~82 |
| Validate | ~24 |
| Enqueue | ~340 |

These three are the most trustworthy figures here: they count instruction work per
order rather than wall-clock contention, and they held within ~4% across runs on a
loaded machine. `epoll_wait` and `read` dominate the total (tens of thousands of
cycles/order) and are *not* quoted, because those are exactly the kernel-path costs
that a loaded, non-isolated box distorts.

---

## Capacity Scaling Matrix

The matrix sweeps load (1M–10M msgs/sec) × gateway threads (1/2/4/8), recording per row the
TCP path, engine cycles, end-to-end latency, and the accepted/rejected split.

### Ingest throughput (measured)

`scripts/measure_throughput.py` sweeps workers × concurrent clients and writes
`benchmark_results.txt`. It samples `orders_in` from the stats region rather than
trusting the client's reported rate — `send()` returns once the data is buffered, so
client-side "throughput" is offered load, not work the engine did. (The tester
happily reports ~2.6M orders/s while the engine consumes ~0.2M.)

| Gateway workers | Concurrent clients | Ingest (orders/sec) |
| ---: | ---: | ---: |
| 1 | 1 | ~207,000 |
| 4 | 1 | ~215,000 |
| 4 | 2 | ~587,000 |
| 4 | 4 | **~1,081,000** |
| 4 | 8 | ~1,068,000 |

Two things fall out of this:

**`SO_REUSEPORT` shards by connection, not by packet.** 4 workers with a single
client performs the same as 1 worker (215k vs 207k) because the accepted connection
is pinned to one worker and the other three idle. Multi-connection load is a
prerequisite for gateway scaling, and any load generator that opens one socket will
silently measure a single worker.

**Ingest saturates around 4 concurrent clients** on this machine (1.08M → 1.07M from
4 to 8), consistent with oversubscription once gateway workers, engine shards, and
the load generators together exceed the available cores.

> **This is a lower bound, not a capacity figure.** The run had no `SCHED_FIFO`
> privilege, a `powersave` governor, and other applications running; the environment
> is recorded in the header of `benchmark_results.txt`. Treat the *shape* as the
> result and re-run the script on isolated hardware for a real ceiling.
>
> **End-to-end latency: the harness is complete; the *numbers* remain
> `TODO(measure)`.** `run_sharding.sh` now runs with `LATENCY_PROFILE=1` and the
> gateway emits e2e (`t1→t5`) and TCP-path (`t4→t5`) P50/P99/P99.9 alongside the
> engine's matching-latency window, all in `results.txt`. But this box cannot grant
> `SCHED_FIFO` (`ulimit -r` is 0) and runs a `powersave` governor, so the tail
> describes the Linux scheduler, not the engine — the same class of misleading figure
> this project already removed once (review B9). The measured values here are a
> *lower bound*; publishable figures need an isolated box. The 1M–10M msgs/sec ×
> threads matrix likewise stays open.

### Reproducing the full matrix (one idle box away)

Every quantity the matrix needs is now produced by a documented command — only the
hardware is missing:

```bash
# Latency percentiles + cycle attribution + accepted/rejected, per shard count.
# Re-run across the thread axis for the matrix rows:
for gt in 1 2 4 8; do GATEWAY_THREADS=$gt ./scripts/run_sharding.sh; done   # -> results.txt

# Ingest throughput sweep (workers x concurrent clients):
python3 scripts/measure_throughput.py                                       # -> benchmark_results.txt
```

On a box that grants `SCHED_FIFO` (`ulimit -r unlimited`), pins to isolated cores
(`isolcpus=`), and uses the `performance` governor, these same two commands turn the
lower bounds above into the publishable matrix. No code change required — the exact
setup steps and how to verify each are in
[`benchmark-setup.md`](./benchmark-setup.md).

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
review A2). Ingest throughput for the gateway is measured above; end-to-end latency for
this run is `TODO(measure)`.

---

## Key Findings

*Items 1, 4 and 5 are measured (see the table above). Items 2 and 3 concern latency and
remain design predictions — this box cannot measure them, see the `SCHED_FIFO` note.*

1. **Single-connection ceiling is real, but it is not an `epoll` limit.** A single
   client tops out near 207–215k orders/sec whether the gateway runs 1 or 4 workers.
   The cause is connection-level sharding, not the ingestion loop: `SO_REUSEPORT`
   pins an accepted connection to one worker. Concurrency has to come from
   connections.
2. **Queueing Delay Dominates Latency:** under saturation the engine logic is expected
   to stay flat while TCP queueing delay balloons — queueing, not execution, dictating
   end-to-end latency. Unverified here (`TODO(measure)`).
3. **SO_REUSEPORT Scalability:** spreading ingress across workers is expected to cut
   queueing delay substantially. Its *throughput* effect is confirmed (5× from 1 to 4
   clients); its *latency* effect is `TODO(measure)`.
4. **The 4-Thread Operating Point:** four gateway threads with four concurrent clients
   is where ingest peaks on this machine (~1.08M orders/sec), and adding clients past
   that does not help — so the Phase-3.3 default is the right operating point here.
5. **Thread Oversubscription:** confirmed directionally — going from 4 to 8 concurrent
   clients slightly *reduces* ingest (1.08M → 1.07M) as gateway workers, engine shards
   and load generators contend for the same cores.

## Conclusions
The functional goal — a documented run that actually matches orders through the full
pipeline — is met (`results.txt`: 4 threads, 10,000 orders, 20,000 fills, 0 rejects).
Gateway ingest is measured at **~1.08M orders/sec** on a developer desktop
(`benchmark_results.txt`), scaling 5× with concurrent connections and saturating at
four. The latency campaign stays open: it needs a box that can grant `SCHED_FIFO`.

The design thesis is supported so far and unchanged: application-side work per order is
small and stable (~82/24/340 cycles for decode/validate/enqueue) while the kernel
networking path dominates the per-order total, which is what motivates the future
kernel-bypass work (DPDK / ef_vi).
