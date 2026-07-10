# Known Issues in the Current HFT Engine

Findings from a read-through of `hft_engine` (and the relevant `hft-trading-firm`
interfaces). Ordered by severity. Line references are to the state of the tree at the
time of writing.

Severity legend: **[CRITICAL]** correctness/UB · **[HIGH]** breaks under advertised
config · **[MED]** scaling/stability · **[LOW]** cleanup/observability.

---

> ✅ **RESOLVED** (this branch). Each producer now owns its own SPSC queue and the
> consumer fans in: engine ingress queues are per-`[shard][gateway-worker]`
> (`Engine` drains its row), and gateway rejects go to per-worker
> `gw_reject_queues` drained by the `OrderManager`. The engine's own drop-copy
> queue is now engine-only. Verified end-to-end under `GATEWAY_THREADS=4`: audit log
> contained exactly 1,256,000 events (256k NEW + 744k REJECT + 256k CANCEL) with zero
> loss/duplication. Original analysis retained below.

## [CRITICAL] Multiple producers push to single-producer (SPSC) queues

`LockFreeQueue` (`core/lock_free_queue.h`) is a **single-producer, single-consumer**
ring. Its `push()` does a non-atomic `buffer[t] = item` then a `tail.store(release)`.
Two threads calling `push()` concurrently both read the same `tail`, both write the
same slot, and one update is lost / the ring is corrupted. Two places violate this:

1. **Drop-copy queues — happens in the DEFAULT config.**
   `drop_copy_queues[shard]` is written by **two different threads**:
   - the matching engine shard, via `OrderBook::send_drop_copy` (`matching/orderbook.h:125`), and
   - the gateway, on a pre-trade reject, via `drop_copy_queues[shard]->push(...)` (`gateway/tcp_server.h:243` and `:371`).

   Even with `GATEWAY_THREADS=1` these are distinct threads pushing to the same SPSC
   queue → data race / lost or corrupted drop-copy records → wrong reject/fill counts
   and a corrupt `order_audit.log`.

2. **Engine task queues — happens under the advertised 4-thread gateway.**
   With `GATEWAY_THREADS>1` (`tcp_server.h:36`, the "4-thread SO_REUSEPORT sharding"
   the README/résumé advertise), any gateway worker can receive an order for any
   instrument and pushes to `queues[inst % NUM_SHARDS]` (`tcp_server.h:254`). Different
   workers thus push to the **same** engine queue → multi-producer on an SPSC ring.

**Impact:** silent order loss / duplicated or torn `EngineTask`s, corrupted book state,
non-reproducible results — precisely under the flagship benchmark configuration.

**Fix options:** give each producer its own SPSC queue and let the consumer fan-in
(N queues per shard), or switch these specific queues to an MPSC design, or serialize
rejects through the owning engine instead of pushing from the gateway.

---

> ✅ **RESOLVED** (this branch). `next_internal_id` is now `std::atomic` (`fetch_add`),
> and each session-map slot is a single `atomic<uint64_t>` packing
> `(internal_id << 16) | instrument_id` — tear-free assign/lookup with no lock
> (internal_id is bounded by 20M < 2^48). Original analysis retained below.

## [CRITICAL] Data race on `SessionManager::next_internal_id` under multi-threaded gateway

`assign_internal_id` does a plain `next_internal_id++` and writes
`client_to_internal[client_order_id] = ...` (`gateway/session_manager.h:31-37`) with no
synchronization. With `GATEWAY_THREADS>1`, multiple worker threads share the one
`SessionManager` instance (`tcp_server.h:454`) and call this concurrently:

- `next_internal_id++` is read-modify-write → **duplicate internal IDs**, which then
  collide in every shard's `orders_by_id[internal_id]` map and in the recycle logic.
- Concurrent writes to the `client_to_internal` array tear.

**Impact:** duplicate/again UB IDs → cancels hit the wrong order, fills attribute to the
wrong client, `orders_by_id` corruption. Only safe at `GATEWAY_THREADS=1`.

**Fix:** make `next_internal_id` atomic (`fetch_add(relaxed)`), or shard the ID space
per gateway thread (e.g. high bits = thread id), or move ID assignment into the engine.

---

> ✅ **RESOLVED** (this branch). `find_next_ask` / `find_next_bid` now use
> boundary-safe L2 masks that keep bits *strictly* above/below the current word
> (special-casing the group boundary so the current word can't be wrongly
> re-selected, and avoiding the shift-by-64 UB). Note: the sharper diagnosis is that
> the mask boundary error `(word_idx±1) & 63` could return a price on the *wrong side*
> of the start; a true `clz/ctz(0)` requires the L2/L1 invariant to also break. Both
> are now closed. This ladder still has **no unit tests** — see the note at the end.

## [CRITICAL] Undefined behavior from `__builtin_clzll(0)` / `ctzll(0)` in book navigation

In `find_next_bid` (`matching/orderbook.h:90-109`) the L2-summary fast path can compute
`63 - __builtin_clzll(bids_bitmap[next_word_idx])` where `bids_bitmap[next_word_idx]`
may be `0`. `__builtin_clzll(0)` (and `__builtin_ctzll(0)` on the ask side) is
**undefined behavior** and returns garbage on x86 when the input is zero.

Concretely, the mask `~0ULL >> (63 - ((word_idx - 1) & 63))` does not correctly exclude
the already-checked current word when `word_idx == 0` (the `& 63` wraps `-1` to `63`,
producing an all-ones mask), so the code can re-select a word whose L1 bitmap is empty
and then call `clzll` on `0`.

**Impact:** intermittent wrong best-bid/ask, potential out-of-range price index, hard to
reproduce. Latent because the benchmark's price distribution rarely hits the empty-word
boundary.

**Fix:** guard the L2 fast path against zero L1 words and add unit tests for the
word-boundary and empty-book cases (this whole bitmap ladder currently has **no tests**).

---

## [HIGH] Gateway worker threads are neither pinned nor real-time — contradicts the design claims

`set_realtime_priority` is applied to the *outer* `server_thread` (`app/exchange.cpp:100-104`),
but that thread then spawns `num_threads` worker threads inside `TCPServer::run`
(`tcp_server.h:193-198`). Those workers are **new threads** — they do **not** inherit
the CPU affinity or `SCHED_FIFO` priority, and nothing pins them. So the SO_REUSEPORT
gateway shards float across cores at normal priority, contradicting the "pinned to
isolated cores" architecture and hurting tail latency/determinism.

**Fix:** call the pinning/priority setup inside `worker_loop` per worker, with a
per-worker core from the config map (§6 of the prep doc).

---

> ✅ **RESOLVED** (this branch). `internal_id` is now the **memory-pool slot index**
> (which recycles) instead of a monotonic counter, so it never exhausts. `orders_by_id`
> shrinks to pool capacity (`POOL_CAPACITY_PER_SHARD`, 500k) — 640MB→16MB total — and
> `orders_by_id[slot]` is `nullptr` exactly when the slot is free, so inserts never
> collide. Stale cancels (a cancel whose slot has since been recycled to a different
> order) are rejected by validating `client_order_id` in the engine. Slot 0 is reserved
> as the null handle so `internal_id == 0` still means "none". Verified: 10 back-to-back
> 1M-order rounds (2.56M allocations vs a 500k/shard pool — impossible without recycling)
> completed with an exact 12,560,000-event audit log and zero exhaustion.
>
> A **related concurrency bug** was found and fixed in the same pass: `MemoryPool::allocate`
> is called by the gateway, so under `GATEWAY_THREADS>1` multiple workers raced on
> `high_water_mark++` and turned the recycle ring into a multi-consumer SPSC violation.
> `allocate()` now serializes its fast path with a spinlock (the engine's `deallocate()`
> stays lock-free). Original analysis retained below.

## [MED] `internal_id` / `orders_by_id` exhaustion caps lifetime orders at ~20M

`next_internal_id` grows monotonically for the whole session and is used to index each
shard's `orders_by_id` array of size `MAX_ORDERS_LOOKUP = 20,000,001`
(`matching/orderbook.h:11`, `matching/engine.h:30`). `match_order` silently
`deallocate`s any order whose `internal_id >= MAX_ORDERS_LOOKUP`
(`orderbook.h:182-185`). The memory *pool* recycles slots, but the ID space does not.

**Impact:** after 20M lifetime orders the engine silently drops every new order — fine
for the 1M-order benchmark, fatal for an "always-on" monitored deployment.

**Fix:** recycle internal IDs alongside pool slots, or use a hash map / wrap-around ID
scheme decoupled from lifetime order count.

---

## [MED] Connection limit of 1024 fds, silently dropped

`accept()` rejects and closes any `client_fd >= MAX_FDS` (1024) per worker
(`tcp_server.h:19`, `:165-168`). On a busy host, fds climb past 1024 quickly (the
process's own listen/udp/epoll/mmap fds count too), so real client connections get
silently refused.

**Fix:** replace the fixed-array `ClientState` indexing with a hash map or dynamically
sized structure keyed by fd, and raise/inspect `RLIMIT_NOFILE`.

---

## [MED] `ClientState` is 128 KB × 1024 × threads — large, sparse, and reset on overflow

Each `ClientState` embeds a `char buffer[131072]` (`tcp_server.h:290`). The array is
`MAX_FDS * num_threads` entries → 128 KB × 1024 = **128 MB per gateway thread**,
allocated up front (`tcp_server.h:41`). Also, on buffer-full the code discards unread
bytes by resetting `read_pos = write_pos = 0` (`tcp_server.h:302-306`), which can
**drop a partially-received message** and desynchronize the byte stream framing.

**Fix:** smaller per-connection buffers allocated on connect; on overflow, `memmove` the
unparsed fragment instead of zeroing (the code already does the correct compaction at
`:430-434` — apply the same logic instead of the hard reset).

---

## [MED] Audit-log validity is only known on clean shutdown

`OrderManager` records `log_index` in process memory and writes it to the file (via
`ftruncate`) **only in its destructor** (`auxiliary/order_manager.h:68`). A crash leaves
a 20M-entry file that looks fully populated (mostly zeros); a live reader can't tell how
many entries are valid. (Fix tracked as §5 in the prep doc.)

---

## [LOW] `g_stats.dropped_reports` is counted but never reported

> ✅ **RESOLVED** (this branch). `g_stats.dropped_reports` is now printed in the final
> shutdown stats (`app/exchange.cpp`). Full machine-readable export still lands with the
> stats region (prep doc §4).

Market-data reports dropped when the ITCH queue is full are counted
(`matching/orderbook.h:118`) but the value is never printed or exported. Silent data
loss in the market-data feed is invisible. (Surface it via the stats region — prep doc §4.)

---

## [LOW] Publisher timestamp: dead match-time write

> ✅ **RESOLVED** (this branch), with a sharper diagnosis than originally written.
> `ItchMessage::timestamp` is the **exchange send time** by design — the firm reads it as
> `t1_exchange_send` to measure send→receive latency
> (`hft-trading-firm/.../LocalExchangeConnector.h:314`), and the Publisher stamps it at
> send time (`publisher.h:54`). So the send-time write is intentional and consumed; the
> real defect was that `broadcast` also wrote an `rdtsc` into that field at match time
> (`orderbook.h`), which was **always overwritten before send** — a dead write on the hot
> matching path. That write is removed. (If event/match time is ever needed by consumers,
> add a separate `egress_tsc` field per prep doc §3 rather than reusing this one.)

`ItchMessage::timestamp` is set at match time in `broadcast` (`orderbook.h:116`) then
unconditionally overwritten with a send-time `RDTSCP` in the Publisher
(`market_data/publisher.h:54`). Downstream consumers can never recover event time.
(Fix alongside prep doc §3.)

---

## [LOW] Shutdown race on `Timer`

> ✅ **RESOLVED** (this branch). This was actually more than a benign Timer race: `main`
> and the still-running `OrderManager` both `pop()` the `tsc_queues`, which is a
> multi-consumer SPSC violation. Shutdown now joins every producer/consumer thread
> **before** the final drain, so `main` is the sole accessor (`app/exchange.cpp`).

At shutdown, `main` drains the TSC queues and calls `timer.add_latency` while the
`OrderManager` thread may still be doing the same (`app/exchange.cpp:123-128` vs
`auxiliary/order_manager.h:104`) — `g_running` is set false but `om_thread` isn't joined
until after. `Timer::add_latency` uses a relaxed atomic counter, so this is a benign
data race on the sample buffer, but it can double-count or drop a few final samples.

**Fix:** join `om_thread` before the final drain, or drain only from one place.

---

## [LOW] Duplicated protocol header will drift

> ✅ **MITIGATED** (this branch). Both copies now carry matching
> `static_assert(sizeof(...) == N)` wire-layout guards, so any divergence fails the build
> instead of silently corrupting decoded messages. Full consolidation into one canonical
> header is still tracked as prep doc §1.

`ItchMessage` / `OuchEnterOrder` are defined identically in two files
(`hft_engine/src/protocol/messages.h` and `hft-trading-firm/src/network/messages.h`).
They are in sync today but nothing enforces it; a change to one silently corrupts the
wire contract. (Consolidation tracked as prep doc §1.)

---

## Not bugs, but worth noting

- **`SCHED_FIFO` prio 99 + busy-spin** (`__builtin_ia32_pause` loops in every engine and
  the gateway) on non-isolated cores can starve kernel/other threads. Requires the
  `isolcpus` setup in `scripts/setup_isolcpus.sh` to be applied; otherwise system
  responsiveness suffers under load.
- **`MAP_HUGETLB` fallback is silent** (`core/memory_pool.h:47-51`): if huge pages
  aren't configured the pool quietly uses 4 KB pages, changing the TLB behavior the
  benchmarks assume. It logs to stderr but nothing records which mode was used — worth
  surfacing in the stats region.
- **No automated tests** exist for the order book, matching, or bitmap navigation
  (`hft_engine/tests/` is effectively empty). The CRITICAL bitmap/UB and multi-producer
  issues above are exactly what a small deterministic test suite would catch.
