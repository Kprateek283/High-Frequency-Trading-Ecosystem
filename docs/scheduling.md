# SCHED_OTHER vs SCHED_FIFO — a measured negative result

Real-time scheduling is standard practice for a latency-sensitive engine, and this
project has requested `SCHED_FIFO` since Phase 3.2. Once the privilege was actually
granted and the result measured, **`SCHED_FIFO` turned out to be slower on this box** —
by 59% at one client.

The configuration is kept and the mechanism is sound. What follows is why it loses here,
and what has to be true before it wins.

---

## The measurement

Same binary, same governor (`powersave`), same `EXCHANGE_CPUSET=0-7`, 9 runs per point,
`scripts/measure_throughput.py`. The only variable is scheduling policy.

| Gateway workers × clients | `SCHED_OTHER` | `SCHED_FIFO` prio 80 | Δ |
| ---: | ---: | ---: | ---: |
| 4 × 1 | **533,282** (spread 10%) | 218,716 (spread 95%) | **−59%** |
| 4 × 2 | **1,185,907** (spread 13%) | 882,715 (spread 86%) | **−26%** |
| 4 × 4 | **1,675,517** (spread 53%) | 1,601,272 (spread 37%) | −4% |

The spread matters as much as the median: at one and two clients, realtime scheduling
took run-to-run variance from ~10% to ~90%. That is the signature of a scheduling
pathology, not of a slower code path.

Realtime was verified per run rather than assumed — the exchange printed
`RT_SCHED: granted=11/11 priority=80` on every one.

---

## Why it loses: three mechanisms, all structural

### 1. `SCHED_FIFO` does not timeslice between equal priorities

This is the dominant effect. Under `SCHED_OTHER`, CFS gives every runnable thread a
share; 11 runnable threads across 8 logical CPUs is oversubscribed but *fair*. Under
`SCHED_FIFO`, a running thread is only preempted by a **strictly higher** priority
thread. A same-priority peer waits until the running thread blocks, yields, or exits.

The engine never blocks:

```c
// matching/engine.cpp
if (!found) {
    g_stats.engine_polls_idle[shard_id].fetch_add(1, std::memory_order_relaxed);
    __builtin_ia32_pause();          // spin, do not yield
}
```

That is a deliberate latency decision — blocking would add wake-up latency to the path
the whole design exists to shorten. But it means an engine shard at priority 80 occupies
its CPU **permanently**. Four shards hold four CPUs, and the remaining seven realtime
threads — four gateway workers, publisher, order manager, dispatcher — contend for what
is left.

At one client the effect is worst, because that client's single connection is pinned by
`SO_REUSEPORT` to one gateway worker: if that worker is the one starved, ingest collapses
for the whole measurement window. Hence 95% spread.

### 2. There are more realtime threads than CPUs

| | |
|---|---|
| Threads requesting realtime | **11** (4 engine + 4 gateway + publisher + order_manager + dispatcher) |
| Logical CPUs in `EXCHANGE_CPUSET=0-7` | **8** (only 4 *physical* — see [`machine-profile.md`](./machine-profile.md)) |
| `isolcpus` | not configured |

Realtime scheduling assumes the realtime set fits. Here it does not, and the OS still has
its own work to place on the same cores.

### 3. RT throttling adds a periodic 50 ms stall

```
kernel.sched_rt_runtime_us = 950000
kernel.sched_rt_period_us  = 1000000
```

Realtime tasks may run at most 950 ms of every 1000 ms. A thread spinning 100% of the
time is therefore descheduled for ~50 ms every second. For throughput that is a ~5%
haircut; for a p99.9 latency figure it is fatal, and it would read as a queueing
pathology in the engine rather than as a scheduler artifact.

The throttle is a safety valve, not a bug: without it, a runaway spinning realtime thread
can make a machine unrecoverable.

---

## The earlier failure: partial realtime is worse than none

Before the above was measured, a run granted realtime to only 7 of the 11 threads. The
gateway workers were skipped, because affinity and scheduling policy had been coupled:

```c
int core = core_for_worker(std::getenv("GATEWAY_CORES"), thread_id);
if (core < 0) return;           // returned BEFORE requesting SCHED_FIFO
...
rt::acquire();
```

With `GATEWAY_CORES` unset — which is how the throughput sweep runs — the spinning engine
shards got priority 80 while the gateway workers feeding them stayed at `SCHED_OTHER`, on
the same CPU set. A classic priority inversion: the producers could not preempt the
consumers that were starving them.

| | 4 × 1 ingest | spread |
|---|---:|---:|
| No realtime at all | 533,282 | 10% |
| **Partial (engines only)** | **264,403** | **96%** |
| Full (all 11 threads) | 218,716 | 95% |

Fixed — the two decisions are now independent. The lesson generalises: **if you cannot
grant realtime to the whole pipeline, grant it to none of it.**

---

## Verifying which policy a run actually used

`pthread_setschedparam` fails silently when `RLIMIT_RTPRIO` is 0, and the benchmark
harness sends the exchange's stderr to `/dev/null`. Before this was instrumented, a run
could be published as "SCHED_FIFO" on the strength of `ulimit -r` in the *harness*
process while every exchange thread ran `SCHED_OTHER`.

The exchange now prints, at the READY barrier and again at shutdown:

```
RT_SCHED: granted=11/11 priority=80     # every thread got it
RT_SCHED: granted=7/11  priority=80     # partial -- inversion, see above
RT_SCHED: granted=0/11  priority=80     # none -- the run is SCHED_OTHER
```

`benchmark_results.txt` records this per sweep. The denominator is as important as the
numerator; a boolean "realtime: yes" would have hidden the 7/11 inversion entirely.

---

## What has to be true before `SCHED_FIFO` wins

In order:

1. **Fix the core map.** `config.env` currently puts every engine shard on the same
   physical core as a gateway worker, and shard 3 on an E-core
   ([`machine-profile.md`](./machine-profile.md)). Realtime scheduling on a bad core map
   just makes the contention deterministic.
2. **Isolate the hot path** — `isolcpus` + `nohz_full` + `rcu_nocbs`, plus moving IRQ
   affinity (wifi interrupts currently land on CPUs 3, 5, 6, 7).
3. **Realtime threads ≤ isolated CPUs.** Eleven realtime threads need eleven dedicated
   CPUs, or the thread count has to come down.
4. **Then decide on the throttle.** `kernel.sched_rt_runtime_us=-1` is only defensible
   once the spinning threads are on isolated cores, with a way back into the machine if
   it goes wrong.

Until step 3 holds, `SCHED_FIFO` converts a fair-share problem into a starvation problem.

---

## Current recommendation

**Benchmark this box with `RT_PRIORITY=0`.** The published figures in
[`benchmarks.md`](./benchmarks.md) are `SCHED_OTHER` for that reason.

```bash
RT_PRIORITY=0 python3 scripts/measure_throughput.py     # what the repo publishes
python3 scripts/measure_throughput.py                   # realtime, prio 80 (slower here)
```

The realtime path is not dead code — it is correct, verifiable, and ready for hardware
that can host it. It is simply not an improvement on a 4-physical-core laptop with no
isolated CPUs, and saying otherwise would repeat the mistake this project has already
corrected twice.
