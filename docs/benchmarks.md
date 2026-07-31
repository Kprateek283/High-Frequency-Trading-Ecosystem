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
| Decode | ~162 |
| Validate | ~53 |
| Enqueue | ~536 |
| ↳ Allocate | ~166 |
| ↳ Record | ~47 |
| ↳ Push | ~229 |

These count instruction work per order rather than wall-clock contention, which is why
they are the figures quoted. The full path also carries `epoll_wait` ~54, `read` ~117 and
egress drain ~18, for **~946 cycles/order** total.

Two claims that used to sit here have been withdrawn by measurement. The first was that
`epoll_wait` and `read` *dominate* the total at "tens of thousands of cycles/order" — they
are 171 cycles combined, 18% of the path, and the application side (Enqueue alone, 57%)
dominates instead. That figure predates the egress-coalescing fix, when one `send()`
syscall per 32-byte ack was charged to the read loop. The second was that these held
"within ~4% across runs": across nine runs at 4×4 the same counters ranged Decode 126–273
and Enqueue 416–1149, roughly ±2×, because per-order cost on this box depends on SMT
placement (see [`bottlenecks.md`](./bottlenecks.md) §10). The medians are stable; the
individual runs are not.

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
| 4 | 1 | ~533,000 |
| 4 | 2 | ~1,186,000 |
| 4 | 4 | **~1,676,000** |

Two things fall out of this:

**`SO_REUSEPORT` shards by connection, not by packet.** A single client pins all load to one worker regardless of `GATEWAY_THREADS`. Multi-connection load is a prerequisite for gateway scaling, and any load generator that opens one socket will silently measure a single worker.

**Ingest is load-generator-bound at 1.68M orders/sec.** The measurement at 4 concurrent clients is the point where the `tester` instances saturate this host's ability to *generate* load — the gateway is ~48% idle and the engine shards ~89% idle at that point — rather than the true ceiling of the engine.

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
   reaches **1.68M orders/sec** (median of 9 runs, 52.7% spread) on this machine.
   Ingest is *still climbing* at four clients — this is not a saturation point, it is
   as far as one laptop can drive it while also hosting the load generators.
5. **The measured ceiling is the load generator.** At the 1.68M point the gateway is
   ~48% idle and the engine shards are ~89% idle, both parked in their empty-poll
   branches. The exchange is pinned to the P-cores, so the `tester` processes run
   on E-cores and cannot generate TCP traffic fast enough to feed a gateway that costs
   ~950 cycles/order. An earlier note here claimed ingest *fell* from 4 to 8 clients
   (1.08M → 1.07M); that was measured through the `orders_in` counter, which sits
   downstream of the engine and was throttled by the OrderManager. It is withdrawn.

## Conclusions
The functional goal — a documented run that actually matches orders through the full
pipeline — is met (`results.txt`: 4 threads, 10,000 orders, 20,000 fills, 0 rejects).
Gateway ingest is measured at **1.68M orders/sec** on a developer desktop
(`benchmark_results.txt`), scaling 533k → 1.19M → 1.68M with concurrent connections and
still climbing at four. The latency campaign stays open: it needs a box that can grant
`SCHED_FIFO`.

The design thesis is supported, but the numbers behind it have changed: application-side
work per order is small and stable (~162/53/536 cycles for decode/validate/enqueue) and
the exchange is no longer the limiting stage at all — it idles waiting for the load
generator. The earlier figures on this page (~82/24/340 cycles, ~1.08M orders/sec) were
taken before five bottlenecks were found and removed, and before the throughput counter
was moved to the engine; see [`bottlenecks.md`](./bottlenecks.md). Kernel-bypass work
(DPDK / ef_vi) remains motivated by the per-order syscall cost, but that case now has to
be argued from a gateway that costs ~950 cycles/order, not from a saturated one.
