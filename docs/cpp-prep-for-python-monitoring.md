# C++ Engine Changes Required Before Adding Python Monitoring Modules

**Status:** Planning
**Scope:** Changes to `hft_engine` (and, where noted, `hft-trading-firm`) that must land *before* Python monitoring/TUI modules are built, so the Python side has a stable, machine-readable contract to consume.

The guiding principle: **the C++ hot path must not be touched by monitoring**. Python taps existing out-of-band feeds and a new read-only shared-memory stats region. Nothing below adds work to the order-matching critical path.

---

## 0. TL;DR — What must change

| # | Change | Why Python needs it | Effort |
| :- | :--- | :--- | :--- |
| 1 | Single canonical wire-schema header | Prevent C++/Python struct drift | S |
| 2 | Harden market-data multicast (TTL/LOOP/IF) | Reliable subscription from Python | S |
| 3 | Add TSC↔wall-clock calibration export | Make timestamps meaningful in Python | S |
| 4 | Add a shared-memory stats/health region (`/dev/shm`) | Expose internal counters Python can't see from ITCH | M |
| 5 | Add a live write-index header to the audit log | Read the log while engine is running / after a crash | S |
| 6 | Externalize configuration (ports, addrs, shards) | Orchestrator + monitor share one source of truth | S |
| 7 | Stable build outputs + run metadata (PID/heartbeat) | Orchestration & liveness detection | S |

Recommended order: **1 → 2 → 3 → 5** (out-of-band, zero engine-logic risk) first, then **4** (the richer instrumented path), then **6 → 7** (orchestration polish).

---

## 1. Establish a single canonical wire schema

**Problem.** `ItchMessage`, `OuchEnterOrder`, etc. are duplicated verbatim in
`hft_engine/src/protocol/messages.h` and `hft-trading-firm/src/network/messages.h`.
Python will re-declare these a *third* time (via `struct`/`ctypes`). Three copies
drift silently and corrupt every decoded message.

Also note the layout hazards Python must match exactly:
- `ItchMessage` is `#pragma pack(push,1)` → deterministic **34 bytes**. Good, but Python must use a packed format string (`<cHHQQIQc`), not native alignment.
- `DropCopyMessage` / `OrderLogEntry` (`auxiliary/order_manager.h:16`) are **not** packed and use `alignas(32)` — their on-disk layout in `order_audit.log` includes padding. Python reading the audit log must replicate that padding precisely.

**Action.**
1. Make `hft_engine/src/protocol/messages.h` the single source of truth; have the firm include it (or symlink) instead of maintaining a copy.
2. Add a `PROTOCOL_VERSION` constant and emit it in the stats region (§4) so Python can assert compatibility.
3. Add `static_assert(sizeof(ItchMessage) == 34, ...)` etc. to lock layout at compile time.
4. Ship a `monitoring/schema/wire.py` generated from (or hand-verified against) this header, with a unit test asserting `struct.calcsize()` matches the `static_assert` values.

---

## 2. Harden the market-data multicast so Python can subscribe reliably

**Problem.** `market_data/publisher.h:25-31` opens a UDP socket and `sendmmsg()`s to
`239.255.0.1:12345` but never sets `IP_MULTICAST_TTL`, `IP_MULTICAST_LOOP`, or
`IP_MULTICAST_IF`. It works today only because of Linux defaults (TTL=1, loopback on).
This is fragile across hosts/interfaces and easy to break.

**Action (in `Publisher::run`, one-time setup, off the hot path):**
```cpp
int ttl = 1;                 setsockopt(udp_fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
int loop = 1;                setsockopt(udp_fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
struct in_addr iface{};      iface.s_addr = htonl(INADDR_ANY);
setsockopt(udp_fd, IPPROTO_IP, IP_MULTICAST_IF, &iface, sizeof(iface));
```
Python then joins with the same pattern already used by the firm's
`udp_listener.h:44-49` (`IP_ADD_MEMBERSHIP` on `239.255.0.1`, bind `INADDR_ANY:12345`,
`SO_REUSEADDR` so both the firm and the monitor can listen simultaneously).

---

## 3. Make timestamps interpretable outside the process

**Problem.** Every timestamp on the wire is a raw `RDTSC(P)` cycle count
(`publisher.h:54`, `orderbook.h:116`). A Python process has:
- no `cycles_per_ns` (it's computed privately in `Timer::calibrate`, `core/timer.h:24`), and
- no anchor mapping TSC → epoch time.

So Python can compute *deltas* between two TSC values but cannot render wall-clock
time or correlate with `psutil`/host metrics.

**Action.** At startup, capture one calibration anchor and publish it in the stats
region (§4):
```cpp
struct TscAnchor {
    uint64_t tsc_at_anchor;     // get_tsc()
    uint64_t unix_ns_at_anchor; // CLOCK_REALTIME in ns
    double   cycles_per_ns;     // from Timer::calibrate()
};
```
Python converts any wire TSC to epoch ns via
`unix_ns_at_anchor + (tsc - tsc_at_anchor) / cycles_per_ns`.
(Assumes an invariant TSC — true on the target CPUs; note it as a documented assumption.)

> Also fix the Publisher overwriting the *match-time* timestamp with a *send-time*
> timestamp (`publisher.h:54`). Either keep both (add an `egress_tsc` field) or stop
> clobbering `ItchMessage::timestamp`; otherwise Python loses event time. This is a
> schema change → do it together with §1.

---

## 4. Add a read-only shared-memory stats & health region

**Problem.** The interesting operational signals are currently **printed to stderr and
then discarded**:
- `OrderManager` prints orders/sec, trades/sec, reject rate (`auxiliary/order_manager.h:115`).
- `TCPServer` prints per-stage cycle attribution only at shutdown (`gateway/tcp_server.h:115`).
- `g_stats.dropped_reports` (`matching/order.h:53`) is incremented but never surfaced.
- Queue depths, memory-pool high-water marks, per-shard load: not exposed at all.

Python cannot derive any of these from the ITCH feed.

**Action.** Publish a versioned, seqlock-protected struct into `/dev/shm/hft_stats`
that each thread updates with **relaxed atomic stores** (no locks, negligible cost).
Python `mmap`s it read-only.

```cpp
struct alignas(64) ShardStats {
    std::atomic<uint64_t> orders_in, fills, cancels, rejects;
    std::atomic<uint64_t> engine_q_depth, dropcopy_q_depth, mktdata_q_depth;
    std::atomic<uint64_t> pool_high_water;
};
struct HftStatsRegion {
    uint32_t magic;            // e.g. 0x48465431 "HFT1"
    uint32_t protocol_version; // §1
    std::atomic<uint32_t> seq; // seqlock: odd = writer in progress
    TscAnchor anchor;          // §3
    std::atomic<uint64_t> heartbeat_tsc; // liveness (§7)
    ShardStats shards[NUM_SHARDS];
    // gateway attribution mirror of tcp_server.h counters
    std::atomic<uint64_t> epoll_cycles, read_cycles, decode_cycles,
                          validation_cycles, enqueue_cycles, orders_processed;
};
```
Notes:
- **Seqlock** so Python reads a consistent snapshot: writer bumps `seq` (odd) → writes → bumps (even); reader retries if `seq` changed or is odd.
- Queue depth is cheaply derived as `(tail - head) & (Capacity-1)`; expose a `size()` method on `LockFreeQueue` (`core/lock_free_queue.h`) — currently only `empty()` exists.
- `MemoryPool` should expose `high_water_mark` (`core/memory_pool.h:24`, currently private) so exhaustion (see known-issues §Scaling) is observable *before* it throws.
- This replaces stderr scraping and is the primary feed for the TUI's health panel.

---

## 5. Make the audit log readable live and crash-safe

**Problem.** `order_audit.log` is an `mmap` of `OrderLogEntry[20M]`
(`auxiliary/order_manager.h`). The valid entry count (`log_index`) lives only in
process memory and is written to the file's size **only in the destructor**
(`ftruncate` at `:68`). So:
- While running, a Python reader can't tell how many entries are valid (the file is a 20M-entry sparse region of mostly zeros).
- On a crash, the `ftruncate` never runs → the log looks 20M entries long.

**Action.** Reserve a small header at the start of the mmap containing an
`std::atomic<uint64_t> write_index` updated after each append. Python reads the header
to bound its scan. This makes live tailing and post-crash replay both work, and feeds
the TUI's trade-tape/replay features directly.

---

## 6. Externalize configuration

**Problem.** Key parameters are hardcoded or half-env:
- TCP `9091`, UDP `9092` (`app/exchange.cpp:83`, `tcp_server.h:78`)
- Multicast `239.255.0.1:12345` (`publisher.h:29`)
- `NUM_SHARDS = 4` compiled in (`exchange.cpp:61`, `tcp_server.h:20`)
- `GATEWAY_THREADS` via env only (`tcp_server.h:36`)
- CPU-core pinning map hardcoded (`exchange.cpp:87-108`)

For a Python-orchestrated system, both sides must agree on these without editing C++.

**Action.** Load a single config file (env-expandable YAML/TOML or plain env) that the
C++ engine, the firm, and the Python orchestrator all read. Minimum keys: ports,
multicast addr/port, shard count, core map, gateway threads, stats-shm path,
audit-log path. This is a prerequisite for §7.

---

## 7. Stable build outputs + liveness metadata

**Problem.** No standard way for a Python supervisor to launch, locate, and health-check
the binaries. Build emits `exchange`, `tester`, `market_maker`, `generate_pcap`,
`replay` (`hft_engine/CMakeLists.txt`) into the CMake build dir; there's no PID file,
no readiness signal, no heartbeat.

**Action.**
1. Pin `RUNTIME_OUTPUT_DIRECTORY` to a known `build/bin/` so the orchestrator has stable paths.
2. On startup, write a PID file and print a single `READY` line on stdout once all threads are pinned and sockets are bound (the orchestrator waits for it).
3. Update `heartbeat_tsc` in the stats region (§4) every ~100 ms from the existing
   `OrderManager` loop — Python treats a stale heartbeat as "engine down/stalled".

---

## What explicitly does NOT need to change

- The matching engine, order book, and lock-free queue internals — Python never touches them.
- The OUCH/ITCH hot-path encode/decode.
- The `t1..t4` latency probes embedded in `OuchEnterOrder` — the firm/gateway cooperation stays as is; Python reads the *aggregates* from the stats region (§4) instead.

---

## Dependency on known bugs

The three **CRITICAL** concurrency/UB bugs in [`known-issues.md`](./known-issues.md) —
multi-producer SPSC writes, the `SessionManager` ID race, and the order-book bitmap
boundary bug — plus a related **`MemoryPool::allocate` race** and the **[MED]
`internal_id` exhaustion** limit, have all now been **fixed** and verified end-to-end
under `GATEWAY_THREADS=4` (including a 10-round recycling stress test). The engine is
therefore safe to run in its advertised multi-gateway configuration while the Python
layer is built on top of it.

Relevant follow-through for monitoring: with `internal_id` now equal to the pool slot
index, §4's `pool_high_water` counter doubles as the "live order headroom" signal, and
note for the Python decoder that **ITCH order-reference ids now recycle** (an id is
reused after its order leaves the book) and are unique per `(shard, id)` rather than
globally monotonic — standard ITCH semantics, but the book-reconstruction logic must
treat an `A`(add) as starting a fresh order for that id.
