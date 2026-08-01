# Engineering Bottlenecks & Debugging

Pushing a userspace TCP exchange to its limits exposes fundamental constraints in operating systems and hardware — and, more often than is comfortable, in the measurement apparatus itself. This document chronicles the engineering challenges encountered during development and the forensic process used to solve them.

## Summary

| Bottleneck | Root Cause | Resolution |
|------------|------------|------------|
| **Multi-ms Latency Spike** | TCP receive-buffer saturation | `SO_REUSEPORT` sharding |
| **EBADF Spin Loop** | Invalid FD mapping | Correct FD ownership |
| **SIGBUS Crash** | Write past mmap'd audit log | Bound writes by `write_index` |
| **Oversubscription** | More active threads than P-cores; hybrid P/E topology | Pin to P-cores, 4-thread operating point |
| **Ack write blocking** | `O_NONBLOCK` fd, partial `send()`/`EAGAIN` | Buffer remainder + arm `EPOLLOUT` |
| **Engine throttled to 15% capacity** | Telemetry queue push *spun* instead of dropping | Drop-on-full, matching every other queue |
| **Every throughput number was wrong** | Counter lived 3 hops downstream of the engine | Measure at the engine (`engine_orders_in`) |
| **Egress wall** | One `send()` syscall per 32-byte ack | Coalesce into the out-buffer, flush once |
| **Allocator contention** | One `atomic_flag` per shard pool, all workers | Per-worker slices + SPSC recycle queues |
| **Bimodal cycles/order** | Gateway workers sharing one physical core's SMT siblings | *Unresolved:* physical-only pinning cuts cycles/order but costs throughput |

> **Two eras of figures.** Stories 1–5 record the *original* debugging episodes on a
> pre-fix, unoptimised engine; their cycle counts are historical and their diagnoses
> stand on their own. Stories 6–9 are from the 2026-07 measurement campaign and their
> figures correspond to the current [`benchmarks.md`](./benchmarks.md) matrix. Where an
> earlier conclusion was later falsified, it is marked **withdrawn** rather than deleted.

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
Having settled on 4 Gateway threads, we attempted to scale the system further by enabling 8. Paradoxically, adding more threads *increased* the end-to-end latency and degraded the stability of the Trading Engine.

### The Investigation
We observed the cycle counts in the SPSC Lock-Free Queue and the Matching Engine. At 8 threads, the cycle counts became highly erratic, exhibiting massive standard deviation spikes that suggested the threads were not running continuously.

### The Discovery
The underlying hardware was an Intel Core i5-1240P. This processor provides fewer physical execution resources than the total number of concurrently active gateway, matching, and load-generation threads.

This physically oversubscribed the CPU. The Linux OS scheduler was forced to preempt and migrate threads more frequently, increasing scheduling overhead and reducing cache locality.

**Addendum (2026-07): the topology matters more than the thread count.** The 1240P is a
*hybrid* part — 4 P-cores with SMT (CPU 0–7) and 8 E-cores (CPU 8–15) — and nothing in the
original diagnosis accounted for that. Three consequences went unnoticed for the entire
first measurement campaign:

* `config.env` isolated cores **1–8**, which straddles the P/E boundary and put one engine
  thread on an E-core with materially lower IPC and no SMT.
* Threads that spilled past the P-cores landed on E-cores non-deterministically, producing
  run-to-run swings of 40–80% that were repeatedly mistaken for signal.
* `CPU(s) scaling MHz: 24%` — the powersave governor was holding the chip near a quarter of
  its maximum clock for every measurement taken. Because `rdtscp` reads the *invariant* TSC,
  which ticks at a fixed nominal rate regardless of core frequency, a downclocked core
  inflates cycles-per-order for identical work. Several "contention" signals were this.

Pinning the exchange to the P-cores (`taskset -c 0-7`) eliminated the 4-client throughput
dip entirely. **Withdrawn:** the earlier claim that going 4→8 clients *reduces* ingest was
an artifact of E-core spillover plus a downstream counter (see story 7), not a real effect.

### The Resolution
A 4-shard configuration is the established operating point for this machine (whether it beats 8 shards on the re-measured matrix is `TODO(measure)`). Thread affinity is **already implemented**, not future work: engines, publisher, gateway, and OrderManager are pinned via `pthread_setaffinity_np` + `SCHED_FIFO` (`app/exchange.cpp`), and Phase 3.2 extended pinning to the spawned gateway *worker* threads that previously floated (the last open item in [`known-issues.md`](./known-issues.md)). Scaling further would require a dedicated high-core-count server processor (AMD EPYC / Intel Xeon) with enough isolated cores to avoid oversubscription in the first place.

---

## 5. Non-Blocking Ack Egress & `EPOLLOUT` Backpressure

### The Problem
The private-ack channel makes the gateway a *writer* for the first time: it sends an
`OuchExecutionReport` back on each client's own fd. But those fds are `O_NONBLOCK` +
`EPOLLET`, so a naive `write()` can return `EAGAIN` or a **partial** byte count the moment
the client is slow to read and the kernel send buffer fills. A blocking retry loop would
stall the whole worker's `epoll` loop — the one thing this architecture cannot afford — and
a client that closed mid-stream would raise `SIGPIPE` and kill the exchange.

### The Investigation
The send path is symmetric to the read path but with the failure modes reversed: reads
drain until `EAGAIN` means *empty*; writes push until `EAGAIN` means *full*. The worker
cannot spin waiting for the buffer to drain, because it must keep servicing every other fd
and draining its `ack_queues` column. The kernel already signals writability — `EPOLLOUT` —
so the fix is to hand the waiting back to `epoll` instead of looping in userspace.

### The Resolution
Each `ClientState` carries an out-buffer. To send: append the 32-byte report and try to
flush with `send(fd, …, MSG_NOSIGNAL)`. On a full send → done. On partial/`EAGAIN` → keep
the remainder and **arm `EPOLLOUT`** on that fd. When the fd next fires `EPOLLOUT` → flush
the remainder; when it empties, **disarm** back to `EPOLLIN|EPOLLET`. `MSG_NOSIGNAL`
suppresses `SIGPIPE` on a client that vanished mid-stream. If a client never reads and its
out-buffer overflows, the connection is closed — backpressure of last resort (a slow client
cannot be allowed to grow exchange memory without bound). This trades a rare disconnect for
a bounded, non-blocking writer; it does **not** affect matched/recorded results, which flow
down the independent drop-copy/audit path.

> **Update (2026-07): the `EPOLLOUT` branch is now covered by an automated test.** The
> earlier note here claimed a socketpair "can't easily fill the send buffer to exercise it."
> That was wrong — it can. `tests/test_ack_coalescing.cpp` sets `SO_SNDBUF` to 1024 (the
> kernel clamps it up to 4,608 bytes) and pushes 1,000 × 32-byte acks = 32,000 bytes
> through it. 32 KB cannot clear a 4.6 KB socket buffer in one `send()`, so the partial
> write, the `memmove` compaction of the remainder, the `EPOLLOUT` arm, and the resumed
> flush are all exercised on every run. The test asserts all 1,000 acks arrive intact **and
> in order**, so a botched compaction surfaces as corruption rather than silent loss.

---

## 6. Telemetry Backpressured the Matching Engine

### The Problem
The matching engine appeared to process orders at roughly **280k/sec** across four shards.
The same `OrderBook::match_order` code, benchmarked in-process with no sockets
(`src/tools/bench_orderbook.cpp`), sustained **24M ops/sec** at a p50 of ~53 cycles. An 85×
discrepancy between the same function measured two ways.

### The Investigation
If the engine were genuinely slow, its cycles would be *spent* somewhere. They weren't: at
280k/sec each shard had ~30,000 cycles of wall-clock budget per order against ~53 cycles of
book work. The engine was not computing. It was waiting.

The engine loop pushes to four queues per order — ITCH market data, drop copy, private acks,
and the TSC latency tuple. Three of them are explicitly drop-on-full, with the rationale
recorded in `matching/orderbook.h`:

> *One slow client's worker queue must not stall the whole shard's matching; a lost private
> ack is recoverable, a stalled engine is not.*

### The Discovery
The fourth one spun. `matching/engine.cpp`:

```cpp
TscTuple t = {task.ingress_tsc, get_tsc()};
while (!tsc_queue.push(t)) { __builtin_ia32_pause(); }
```

`tsc_queue` holds 1,048,576 entries and is drained by the single `OrderManager` thread — the
same thread that writes the mmap'd audit log and updates every stats counter. Blast 1M
orders, the queue fills, and from that instant **the matching engine's throughput is
definitionally the OrderManager's drain rate.**

The backpressure propagated the whole way up: engine stalls → `engine_queues` fill → the
gateway spins on its own blocking push in `gateway/tcp_server.h` → the client blocks in
`send()`. Every symptom observed downstream — idle gateway, blocked client, "slow engine" —
was the tail of one spin loop on a *telemetry* queue.

### The Resolution
Drop-on-full with a counter, identical to the three queues beside it. A dropped latency
sample costs one point in a histogram; a stalled engine costs everything. The principle was
already written down in this codebase — it simply hadn't been applied to the fourth site.

---

## 7. The Counter That Measured the Wrong Thing

### The Problem
Every throughput figure the project had ever published came from `measure_throughput.py`,
which read `orders_in` from the stats region. The script's own docstring explained that it
sampled the stats region *rather than* trusting a client's send rate, because a `send()`
returns once buffered and client-side "throughput" is offered load, not work done. The
reasoning was sound. The counter was not.

### The Investigation
Tracing where `orders_in` is actually incremented:

```
gateway → engine_queue → engine → drop_copy_queue → OrderManager → orders_in++
```

It is incremented in `auxiliary/order_manager.h`, **three hops downstream of the engine**, by
the same single thread that was throttling story 6.

### The Discovery
The benchmark had never measured the gateway (as its docstring claimed) or the engine. It
measured the audit logger — the slowest stage of the pipeline — and reported that as system
throughput. Worse, `orders_in` is fed from a queue that is explicitly drop-on-full, so
whenever the drop-copy queue overflowed the counter **silently undercounted**. The published
figure was biased low by an unknown amount, for a reason nobody had written down.

This is why story 6 stayed hidden for so long: fixing the engine would not have moved the
number, because the number was generated downstream of the fix. The instrument was inside
the fault.

### The Resolution
`engine_orders_in` in `core/stats_region.h` — one `alignas(64)` atomic per shard,
incremented by the matching engine itself in its hot loop, so the counter cannot be
throttled by anything downstream of what it measures. `measure_throughput.py` and
`monitoring/wire.py` read it instead.

**The transferable lesson:** when a pipeline stage looks slow, verify *which* stage
increments the counter you are reading before believing anything it tells you.

---

## 8. One Syscall Per 32-Byte Ack

### The Problem
With the engine unblocked, ingest still capped near 1.14M orders/sec — while the gateway
reported ~58% idle and the engine >90% idle. Every stage claimed to be waiting for work, yet
throughput refused to climb. In a pipeline where every stage is idle, the cap is not in any
stage being measured.

### The Investigation
The per-order attribution covered the *ingest* path only: `epoll_wait` → `read` → decode →
validate → enqueue, totalling ~1,819 cycles. But at 286k orders/sec/worker the budget was
~7,387 cycles/order. Subtracting measured work and measured idle left **~1,300 cycles
unaccounted** — a region the instrumentation simply did not cover.

That region was the egress half of the same worker loop: draining `ack_queues` and calling
`send_report`.

### The Discovery
`send_report` issued one `send()` syscall per 32-byte execution report, and a fill generates
two acks. Instrumenting it showed **480 to 4,000+ cycles/order** — on its own larger than
the entire instrumented ingest path, and invisible in both the "work" and "idle" columns.

### The Resolution
Append reports into the existing per-connection `out_buf` and flush once per drain (or when
the buffer fills), reusing the `EPOLLOUT` backpressure machinery from story 5 unchanged. The
egress cost fell to **15–50 cycles/order**, roughly a 100× reduction on that path, and
median throughput moved from 1.14M to 1.75M in the same sweep. Coverage for the partial-flush
path is `tests/test_ack_coalescing.cpp` (see the update in story 5).

---

## 9. The Allocator Spinlock

### The Problem
With engine and egress both unblocked, per-order attribution finally showed a *contended*
signal rather than an idle one: `Allocate` cost 535 cycles/order at four clients, spiking to
1,436 under load, against 148 cycles for `Record` — an adjacent operation on the same code
path that physically cannot contend (a single relaxed atomic store into a shared array,
where firms occupy disjoint token ranges).

### The Investigation
`Record` is the control variable. Both operations are a handful of instructions plus a
memory write; a 3.6× gap between them is not explained by the work either one does. Every
`MemoryPool::allocate` took a single `std::atomic_flag` per shard pool, and all four
`SO_REUSEPORT` gateway workers hit it on every NEW order.

An earlier attempt to pin this down was **inconclusive and nearly led to the wrong fix**: at
that time all three sub-measurements grew together with client count, including the
uncontended control, because the hybrid-CPU effects of story 4 were inflating everything
uniformly. Contention was only separable from topology noise once the exchange was pinned to
the P-cores.

### The Discovery
Ownership of a pool slot is derivable from the slot index alone. That makes the lock
unnecessary rather than merely reducible: partition each shard's pool into contiguous
per-worker slices, and `owner = index / slice_capacity` recovers the owner with no side
table, no per-order metadata, and no extra field in `Order`.

### The Resolution
Each worker allocates from its own slice with **no atomic on the fast path**, and each
`(shard, worker)` pair gets its own SPSC recycle queue — producer is the shard's engine
thread (the sole caller of `deallocate`), consumer is the owning worker. `Allocate` fell to
**198 cycles/order**. Replacing the single 67 MB inline recycle queue with per-worker 2 MB
queues also reclaimed **~235 MB** of resident memory across four shards.

The critical invariant is that `internal_id == pool slot index`, which is load-bearing across
the session manager, drop copy, ITCH, and `orders_by_id`. A slot issued to two workers at
once would not crash — it would produce one wrong fill, days later, under load, without
reproducing. That risk is why the change ships with `tests/test_pool.cpp` exercising slice
containment, concurrent double-issue detection across four threads, recycle routing, and
per-worker exhaustion, all clean under **ThreadSanitizer**.

---

## 10. SMT Siblings, and a Trade-Off That Does Not Resolve

### The Problem

Re-running the sweep to refresh the published figures produced a `Total/Order` that would
not settle. Across nine runs at the 4-worker × 4-client point it did not scatter around a
mean — it split cleanly into two clusters, five runs at 742–1056 cycles/order and three at
1601–1861, with nothing in between. A 2× bimodal distribution is not measurement noise.

### The Investigation

The obvious suspect was §4's frequency story: a downclocked core inflates cycles-per-order
for identical work, because the TSC ticks at a fixed nominal rate regardless of what the
core is actually doing. That explanation was available and wrong. The gateway prints its
calibrated TSC every run, and it read **2.11 cycles/ns in all nine** — including both
clusters. Constant TSC means the extra cycles were really executed, not an artifact of the
clock. Something was making the same work cost twice as much.

`taskset -c 0-7` had been described throughout this project as "pinned to the four
P-cores." On this part that is eight *logical* CPUs — four physical P-cores plus their SMT
siblings — and the exchange runs roughly ten threads across them. Whether two gateway
workers land on two halves of the same physical core is decided by the scheduler, freshly,
on every run. Two workers sharing one core's execution ports would roughly double per-order
cost, and would do so on some runs and not others.

### The Discovery

Re-pinning to one thread per physical P-core (`EXCHANGE_CPUSET=0,2,4,6`) removes the high
cluster outright — median 946 → 697 cycles/order, maximum 1097 against the previous 1861.
The SMT explanation holds.

It is still not a fix. The same change, measured across the full sweep:

| | `0-7` (P-cores + siblings) | `0,2,4,6` (physical only) |
|---|---:|---:|
| 4 workers, 1 client | 533,282 | 251,894 |
| 4 workers, 2 clients | 1,185,907 | 939,927 |
| 4 workers, 4 clients | 1,675,517 (±52.7%) | 1,828,434 (±64.5%) |
| median cycles/order | 946 | 697 |

Single-client throughput halves. Two-client drops 21%. The 4-client points overlap well
inside their spreads and cannot be called apart. Four logical CPUs simply cannot host ten
threads, so the cycles each order costs go down while the number of orders per second goes
down with them.

### The Resolution

None — and that is the entry. **Cycles/order and orders/second are different quantities,
and here they move in opposite directions.** Optimising the first would have made the
system worse at the second. The SMT-inclusive layout remains the published configuration
because throughput is what the benchmark claims to measure; the efficiency win is recorded
and left on the table. (`EXCHANGE_CPUSET` has since defaulted to `0-10` rather than `0-7`,
which adds E-cores 8–10 for the three cold aux threads and leaves the P-core layout under
test here unchanged. Both arms above were measured on the pre-`config.env` harness — see
`benchmark_results.txt` — so their absolute numbers predate the current sweep, but the
comparison between them holds.)

An earlier draft of this measurement ran six iterations at the 4×4 point only, saw
1.95M vs 1.68M, and concluded physical-core pinning was a 16% win. Nine runs across the
full client sweep withdrew it. The failure mode is the one this document already records
twice: a real effect, measured at one operating point, generalised to a claim the data did
not cover.

### Postscript — the bimodality does not reproduce

Nine fresh runs after the harness fix (`config.env` actually applied, load generators
pinned off the measured cores, corrected core map) measure `Total/Order` at **398–500,
unimodal, median 433** — against 742–1861 bimodal before. The two clusters are gone, and
so is roughly half the absolute cost.

The SMT reasoning above is not withdrawn: two workers sharing one core's execution ports
*would* produce exactly that signature, and the `0,2,4,6` arm did remove the high cluster.
But the fresh data admits a second explanation that was not controlled for. `__rdtscp`
counts **elapsed** cycles, not retired instructions, so an unpinned load generator
competing for the same physical core inflates "cycles/order" without the gateway executing
anything extra — and until the harness fix, the generators were doing exactly that on
every run.

Two variables moved together here (generator placement and the core map), so this is
recorded as **not attributed**. What can be said: the bimodality was at least partly a
measurement artifact rather than purely a property of the code, and any future use of
these counters has to control generator placement first.

---

## What Was *Not* the Bottleneck

Four hypotheses were investigated and eliminated by measurement. They are recorded because
the eliminations cost as much effort as the fixes, and because each one looked convincing
enough to act on:

| Hypothesis | Why it was plausible | How it was eliminated |
|---|---|---|
| **NVMe writeback on the audit log** | `OrderManager` writes an mmap'd, disk-backed file per message; `balance_dirty_pages` throttles writers once the dirty ratio is hit | Pointed `AUDIT_LOG_PATH` at `/dev/shm` (tmpfs, no writeback). Throughput unchanged at ~265k. Not disk. |
| **Load generator throttled** | `tester.cpp` defaults `TARGET_RATE` to 1,000,000/sec — suspiciously close to the then-published 1.08M | `measure_throughput.py` sets `TARGET_RATE=0`, and the pacing loop is guarded by `if (target_rate > 0)`. Never engaged. *(The default was still a footgun for manual runs and has since been changed to 0.)* |
| **Missed `epoll` edge** | Client fds are `EPOLLIN\|EPOLLET`; a blocked client plus an idle gateway is the textbook signature of an incomplete drain | Switched client fds to level-triggered, which masks incomplete drains entirely. Throughput unchanged (292k vs 270k). Added a background `recv()` thread to keep the TCP window open — also unchanged. The read loop drains correctly. |
| **Load generator CPU-saturated** | `tester` sat at 4.4% CPU in `S` state — clearly not generating at capacity | It was sleeping, not starved: `tester.cpp` called `sleep_for(1µs)` per batch in its *cancel* phase, parking the process for ~500ms after the initial blast. An artifact of the harness, not the exchange. |

The `EPOLLET` elimination is worth dwelling on. A blocked writer and a sleeping reader
genuinely *is* the signature of a lost edge — the inference was reasonable and it was still
wrong. The level-triggered experiment cost one line and one run, and it closed the question
in a way that no amount of further reading of the event loop would have.
