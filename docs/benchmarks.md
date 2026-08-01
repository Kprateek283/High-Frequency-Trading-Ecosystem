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
*   **OS:** Fedora Linux 44, kernel 6.19.10-300.fc44.x86_64. Full inventory —
    topology, governor, IRQ placement, realtime limits — in
    [`machine-profile.md`](./machine-profile.md).
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

Measured over 9 runs at 4 workers × 4 clients, with the load generators pinned off the
cores under measurement:

| Stage | Cycles/order (median) | Range | Share |
| :--- | ---: | ---: | ---: |
| `epoll_wait` | 41 | 4–101 | 9% |
| `read` | 41 | 39–48 | 9% |
| Decode | 95 | 91–101 | 22% |
| Validate | 17 | 16–22 | 4% |
| Enqueue | 228 | 204–265 | 53% |
| ↳ Allocate | 80 | 67–94 | |
| ↳ Record | 19 | 17–23 | |
| ↳ Push | 84 | 77–104 | |
| Egress drain | 12 | 10–15 | 3% |
| **Total** | **433** | **398–500** | |

Three things about how to read this table.

**These are elapsed cycles, not retired instructions.** `__rdtscp` reads a wall-clock
counter, so a stall, a cache miss, or a preemption all land in the number. An earlier
version of this section claimed the opposite — that the counters "count instruction work
per order rather than wall-clock contention" — and used that to justify quoting them as a
property of the code. It is wrong, and the correction is visible in the table: the same
counters previously read ~946 cycles/order total, and roughly halved once the load
generators stopped competing for the same cores. Nothing in the gateway changed. Contention
was being reported as per-order cost.

**The stage medians sum to 434 against a median total of 433.** That agreement is
coincidence, not arithmetic: `Total/Order` is a true per-run sum in the code
(`gateway/tcp_server.h`), but medians taken across runs need not add up. The previously
published figures did not — stages summing to 940 were reported as a ~946 total.

**Allocate + Record + Push (183) does not equal Enqueue (228), and cannot.** On the
accepted path the three sub-spans tile the enqueue span exactly by construction. But the
risk-reject path (`tcp_server.h:529`) increments `enqueue` without the three sub-counters,
and this sweep's workload rejects 19–86% of orders (see below), so the shortfall is
rejected orders. A related instrumentation gap: the pool-exhaustion path
(`tcp_server.h:543`) increments the order count with *no* cycle counters at all, diluting
every per-order average slightly downward.

One claim previously withdrawn here still stands: `epoll_wait` and `read` do not dominate.
They are 82 cycles combined, 19% of the path, while Enqueue alone is 53%.

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

| Gateway workers | Concurrent clients | Ingest (orders/sec) | Spread |
| ---: | ---: | ---: | ---: |
| 4 | 1 | ~481,000 | 38% |
| 4 | 2 | ~1,095,000 | 72% |
| 4 | 4 | **~2,062,000** | 19% |

Three things fall out of this:

**`SO_REUSEPORT` shards by connection, not by packet.** A single client pins all load to one worker regardless of `GATEWAY_THREADS`. Multi-connection load is a prerequisite for gateway scaling, and any load generator that opens one socket will silently measure a single worker.

**Ingest is load-generator-bound at 2.06M orders/sec.** The measurement at 4 concurrent clients is the point where the `tester` instances saturate this host's ability to *generate* load — the gateway is **~87% idle** and the engine shards **~96% idle** at that point — rather than the true ceiling of the engine. (Those idle figures were previously given as ~48% and ~89%; both were measured while the generators were still competing with the exchange for cores.)

**The figure counts *accepted* orders, not orders received.** `engine_orders_in` is incremented by the matching engine, and risk-rejected orders never reach it — they are answered by the gateway and dropped at `tcp_server.h:529`. Under this sweep's workload (`WORKLOAD_TYPE=2`, uniform-random symbols at unthrottled rate) the observed reject rate runs **19–86%**, climbing through a run as positions accumulate against the pre-trade risk limits. So the gateway is decoding materially more messages per second than 2.06M. Read the number as *engine intake*, which is what the counter measures, and note it is not comparable to the 0%-reject functional run in `results.txt`, which uses a different workload.

**The benchmark used to measure itself.** These figures replace an earlier
533k/1.19M/1.68M sweep, and the difference is a harness fix rather than an engine
change. That harness never applied `config.env` — only the bash entry points source it,
so the exchange silently ran on the fallbacks in `exchange.cpp`, with `GATEWAY_CORES`
unset and therefore the gateway workers unpinned. The `tester` processes were unpinned
too, so the load generators competed with the exchange for the very P-cores under
measurement. Pinning them off those cores (`TESTER_CPUSET`, default `11-15`) accounts for
the entire gain: +31% at four clients, with run-to-run spread falling 77% → 19%. It also
made the 1- and 2-client points ~9% *worse*, because with one or two generators there is
no parallelism to offset running them at 3300 MHz. The corrected core map, measured on
its own, was within noise (−6% against a 35–101% spread). Per-arm attribution is recorded
in `benchmark_results.txt`.

> The 4×2 spread is 72%. Treat that median as indicative only.

> **This is a lower bound, not a capacity figure.** The run had no `SCHED_FIFO`
> privilege, a `powersave` governor, and other applications running; the environment
> is recorded in the header of `benchmark_results.txt`. Treat the *shape* as the
> result and re-run the script on isolated hardware for a real ceiling.
>
> **End-to-end latency: the harness is complete; the *numbers* remain
> `TODO(measure)`.** `run_sharding.sh` now runs with `LATENCY_PROFILE=1` and the
> gateway emits e2e (`t1→t5`) and TCP-path (`t4→t5`) P50/P99/P99.9 alongside the
> engine's matching-latency window, all in `results.txt`. But `SCHED_FIFO`, though now granted, makes
> throughput *worse* here and cannot fix the tail without `isolcpus`
> (see [`scheduling.md`](./scheduling.md)), and the box runs a `powersave`
> governor — so the tail
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

On a box that grants `SCHED_FIFO` (via `/etc/security/limits.d/`, see setup), pins to isolated cores
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

Reject rate is now **0%** — a property of *this* workload fitting the 256-instrument cap
with symbols that decode, not of a parsing failure (contrast the pre-fix 74.4% reject rate
in review A2). Note it is specific to the deterministic crossing driver: the throughput
sweep uses `WORKLOAD_TYPE=2` and rejects 19–86% on pre-trade risk, as described above.

`results.txt` also carries latency figures, and they are **not** the missing end-to-end
measurement — that stays `TODO(measure)`. Three reasons, all recorded in the file itself:
`liquidity` stamps `t1=t2=t3=t4` with one send-time TSC, so three of the five stages read
zero by construction; it blasts 20k orders unthrottled, so `t5−t4` is burst *queueing*
delay rather than per-order network latency; and its gateway cycle attribution is not
comparable to `cycle_attribution.txt`, since a ~95%-idle 20k-order run amortises
`epoll_wait` blocking over a tiny denominator. A populated five-point decomposition needs
the trading firm (`LocalExchangeConnector`), which stamps each stage separately.

---

## Key Findings

*Items 1, 4 and 5 are measured (see the table above). Items 2 and 3 concern latency and
remain design predictions — this box cannot measure them, see the `SCHED_FIFO` note.*

1. **Single-connection ceiling is real, but it is not an `epoll` limit.** A single
   client tops out near **481k orders/sec** whether the gateway runs 1 or 4 workers.
   The cause is connection-level sharding, not the ingestion loop: `SO_REUSEPORT`
   pins an accepted connection to one worker. Concurrency has to come from
   connections. (This line previously read "207–215k", a figure that predated both
   the bottleneck fixes and the move to the `engine_orders_in` counter, and which
   contradicted the sweep table above it.)
2. **Queueing Delay Dominates Latency:** under saturation the engine logic is expected
   to stay flat while TCP queueing delay balloons — queueing, not execution, dictating
   end-to-end latency. Unverified here (`TODO(measure)`).
3. **SO_REUSEPORT Scalability:** spreading ingress across workers is expected to cut
   queueing delay substantially. Its *throughput* effect is confirmed (5× from 1 to 4
   clients); its *latency* effect is `TODO(measure)`.
4. **The 4-Thread Operating Point:** four gateway threads with four concurrent clients
   reaches **2.06M orders/sec** (median of 9 runs, 19.4% spread) on this machine.
   Ingest is *still climbing* at four clients — this is not a saturation point, it is
   as far as one laptop can drive it while also hosting the load generators.
5. **The measured ceiling is the load generator.** At the 2.06M point the gateway is
   ~87% idle and the engine shards are ~96% idle, both parked in their empty-poll
   branches. The exchange is pinned to the P-cores and the `tester` processes to the
   E-cores, which cannot generate TCP traffic fast enough to feed a gateway that costs
   ~433 cycles/order. An earlier note here claimed ingest *fell* from 4 to 8 clients
   (1.08M → 1.07M); that was measured through the `orders_in` counter, which sits
   downstream of the engine and was throttled by the OrderManager. It is withdrawn.
6. **Where the load generators run is part of the measurement.** Leaving them unpinned
   cost 31% at four clients and tripled the run-to-run spread, purely by contending
   with the exchange for the cores being measured. A benchmark that does not place its
   own load generator is measuring the placement, not the system.

## Conclusions
The functional goal — a documented run that actually matches orders through the full
pipeline — is met (`results.txt`: 4 threads, 10,000 orders, 20,000 fills, 0 rejects).
Gateway ingest is measured at **2.06M orders/sec** on a developer desktop
(`benchmark_results.txt`), scaling 481k → 1.09M → 2.06M with concurrent connections and
still climbing at four. The latency campaign stays open — and it now needs isolated
cores rather than merely `SCHED_FIFO` privilege, which is granted here and makes
throughput worse ([`scheduling.md`](./scheduling.md)).

The design thesis is supported, but the numbers behind it have changed: application-side
work per order is small (95/17/228 cycles for decode/validate/enqueue) and
the exchange is no longer the limiting stage at all — it idles waiting for the load
generator. The earlier figures on this page (~82/24/340 cycles, ~1.08M orders/sec) were
taken before five bottlenecks were found and removed, and before the throughput counter
was moved to the engine; see [`bottlenecks.md`](./bottlenecks.md). Kernel-bypass work
(DPDK / ef_vi) remains motivated by the per-order syscall cost, but that case now has to
be argued from a gateway that costs ~433 cycles/order, not from a saturated one.
