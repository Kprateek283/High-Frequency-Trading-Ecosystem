# Senior Review — Findings

A full read-through of `hft_engine`, `hft-trading-firm`, `scripts/`, and `docs/`,
covering three questions:

1. What is actually wrong with the code, ordered big → small?
2. Where do the docs disagree with the code?
3. Where do the docs disagree with each other?

This supersedes nothing — [`known-issues.md`](./known-issues.md) is the earlier team
audit and remains the record for the issues it covers. Section D reconciles it against
the tree as it stands and notes the one class of bug it missed. Line references are to
the state of the tree at the time of writing (branch `fix/engine-concurrency-and-exhaustion`).

Severity legend: **[CRITICAL]** the advertised system does not do what it says ·
**[HIGH]** silent data loss or a headline number that misrepresents the workload ·
**[MED]** crash/robustness · **[LOW]** cleanup.

---

## Resolution status (added after the phased implementation)

Every item below has been addressed by the phased plan
([`implementation-plan.md`](./implementation-plan.md)). Original findings retained
verbatim underneath for the record.

| Item | Resolution | Phase |
| :--- | :--- | :--- |
| **A1** four symbol encodings | one canonical `STK#####` decode in the shared header; all clients emit it | 1.2 |
| **A2** 256 vs 1000 / reject split | `MAX_INSTRUMENTS=256`, tester→256; split published — `results.txt` shows **0% reject** | 1.2 |
| **A3** silent drop-copy loss | `dropped_drop_copies` counter, printed + in stats region | 2.2 |
| **A4** pool throw → `terminate` | `allocate()` returns `nullptr`; order rejected via existing flow | 2.1 |
| **A5** no self-trade prevention | cancel-newest on same-client cross | 2.3 |
| **A6** `client_id` from token | identity assigned server-side from the session | 1.3 |
| **A7** duplicate UDP order path | deleted (no in-repo client used it) | 1.1 |
| **A8** dead code | deleted under `-Werror` | 0.1 |
| **A9** listener LT vs ET | code comment + deep-dive sentence (intentional) | 6 |
| **B1–B6, B10, B11** doc↔code | this documentation truth pass | 6 |
| **B7** run script 4-vs-1 thread | `run_sharding.sh` defaults `GATEWAY_THREADS=4` | 3.3 |
| **B8** README build block | corrected; run from root; real results file | 3.4 |
| **B9** engine `-O3` flags | both projects unified on `-O3 -march=native -flto -Werror` | 0.1 |
| **C1** timestamp count | one five-point story in all three docs | 6 |
| **C2/C4/C6** benchmark numbers | fabricated figures removed; re-measure pending — `TODO(measure)` | 6 / 3.5 |
| **C3** EBADF→SIGBUS | split into two independent bugs (livelock; mmap overrun) | 6 |
| **C5** "Core Business Logic" | renamed "Gateway Ingest Path"; engine quoted separately | 6 |
| **E** empty test suite | plain-assert harness + CTest, unit tests per phase | 0.3+ |
| **E** two build configs | unified (see B9) | 0.1 |

---

# A. Code issues

## A1. [CRITICAL] Four incompatible symbol encodings; the documented benchmark matches zero orders

The gateway decodes the 8-byte OUCH `stock` field with exactly one scheme
(`gateway/tcp_server.h:243-250` on the UDP path, `:385-392` on the TCP path):

```cpp
uint16_t inst = 999;
if (req.stock[0] == 'S' && req.stock[1] == 'T' && req.stock[2] == 'K') {
    inst = (req.stock[3] - '0') * 10000 + (req.stock[4] - '0') * 1000 +
           (req.stock[5] - '0') * 100   + (req.stock[6] - '0') * 10   +
           (req.stock[7] - '0');
}
```

Anything that is not `STK#####` falls through as `inst = 999`. The risk engine then
rejects it, because `999 >= 256` (`gateway/risk_engine.h`, `MAX_INSTRUMENTS`):

```cpp
if (mapped_inst >= 256) return false; // MAX_INSTRUMENTS
```

Four different clients in this repo send four different things:

| Client | Symbol sent | Decodes to | Outcome |
| :--- | :--- | :--- | :--- |
| `hft-trading-firm` (`network/LocalExchangeConnector.h:33-38`, used at `:152`, `:221`) | `INSTR0  ` … `INSTR3  ` | 999 | **rejected** |
| `hft_engine/src/tools/market_maker.cpp:73` | `AAPL    ` | 999 | **rejected** |
| `hft_engine/src/tools/generate_pcap.cpp:20,35` | `AAPL/MSFT/GOOG/AMZN` | 999 | **rejected** |
| `hft_engine/src/tools/tester.cpp:63-69` | `STK00000`…`STK00999` | 0–999 | 0–255 accepted, 256–999 rejected |

Only `tester.cpp` speaks the gateway's language, and even it overshoots the 256 cap.

**Why this is the top finding, not a nit:** `README.md:83` documents
`./scripts/run_sharding.sh` as *the* way to reproduce the benchmark, and that script runs
`trading_firm` — the client that sends `INSTR0..3`. Every order in the advertised
end-to-end run is rejected at pre-trade risk. **The matching engine never matches a single
order on the documented run path.** The throughput number is real (the gateway does ingest
and reject 10M msgs/sec), but it measures decode-and-reject, not the price-time-priority
matching the README is selling.

**Fix:** one canonical symbol→instrument mapping shared by both sides. The cheapest
version that makes the documented run real is to teach the gateway's decoder `INSTR0..3`
alongside `STK#####`; the correct version is a single shared header (see the protocol
duplication item, D4/known-issues `[LOW] Duplicated protocol header`) owning both the
struct layout *and* the symbol table.

---

## A2. [HIGH] The 10M msgs/sec headline is ~74% rejects even on the one client that works

`tester.cpp:43` sets `num_symbols = 1000`; the risk engine caps instruments at 256. Under
the default sequential workload (`sym_id = i % 1000`), 744 of every 1000 orders are
rejected before ever reaching a book.

This is not a hypothesis. `known-issues.md:17` records the verified run itself:

> audit log contained exactly 1,256,000 events (256k NEW + 744k REJECT + 256k CANCEL)

744k/1M = 74.4%, exactly `(1000 - 256) / 1000`. The prior audit quoted this number as
proof that the queue fixes hold (which it is) without remarking that it also proves
three-quarters of the flagship benchmark is a reject path.

**Fix:** pick one and state it. Either raise `MAX_INSTRUMENTS` to 1024 so the benchmark
actually exercises matching, or drop `tester.cpp`'s `num_symbols` to 256 and re-run — then
publish the accepted/rejected split alongside the throughput number in `benchmarks.md`.
The current presentation is not defensible in an interview.

---

## A3. [HIGH] Drop-copy loss is silent; market-data loss is counted

`matching/orderbook.h:150-154`:

```cpp
inline void send_drop_copy(uint64_t client_id, uint64_t internal_id, uint64_t price,
                           uint32_t qty, uint16_t inst, Side side, OrderState state) {
    DropCopyMessage msg = {client_id, internal_id, price, qty, inst, state, side};
    // Don't spin, drop copy queue is huge and read asynchronously.
    drop_copy_queue->push(msg);
}
```

The `push()` return value is discarded. The sibling `broadcast()` twelve lines up does the
right thing (`orderbook.h:146`):

```cpp
g_stats.dropped_reports.fetch_add(1, std::memory_order_relaxed);
```

So a full ITCH queue is counted and reported, while a full drop-copy queue silently
vanishes an execution record. The drop-copy feed is what backs `order_audit.log` — the
same file `known-issues.md` uses to certify "zero loss/duplication". A drop here is
invisible to the very check meant to catch it. "The queue is huge" is a capacity argument,
not a correctness one; the counter costs one relaxed increment on a path that is already
writing 32 bytes.

**Fix:** mirror `broadcast` — `if (!drop_copy_queue->push(msg)) g_stats.dropped_drop_copies.fetch_add(1, relaxed);`
and print it in the shutdown stats next to `dropped_reports`.

---

## A4. [MED] `MemoryPool::allocate` throws on the gateway hot path → `std::terminate`

`core/memory_pool.h:88`:

```cpp
throw std::runtime_error("Memory Pool exhausted!");
```

`allocate()` is called from the gateway worker (`tcp_server.h`), and neither `worker_loop`
nor `handle_client` has a `try`/`catch`. An exception escaping a thread's entry function
calls `std::terminate` — the whole exchange dies, mid-session, with every other connection's
state lost, because one shard ran out of slots.

Pool exhaustion is a *foreseeable* condition on an exchange gateway (a client that opens
500k orders without cancelling is a business event, not a programming error). It has an
obvious, already-implemented answer: this is exactly the reject path the risk engine
already owns.

**Fix:** return `nullptr` from `allocate()` on exhaustion and reject the order with the
existing reject flow. If you keep the throw, it must at minimum be caught at the top of
`worker_loop` — but rejecting the order is both smaller and more correct than tearing down
the process.

---

## A5. [MED] No self-trade prevention

The matching engine matches on price-time priority with no check that the resting order's
owner differs from the incoming order's. A single client can cross with itself and book a
wash trade. Every real venue has STP as a hard requirement (it is a regulatory obligation,
not a feature), and the ecosystem ships a `market_maker` that quotes both sides of the
same instrument — the exact configuration that trips it.

**Fix:** compare `client_order_id`'s owning firm on the resting vs incoming order before
executing; cancel-newest or cancel-oldest per policy. Worth noting as a scope decision in
the README if it's deliberately out of scope.

---

## A6. [MED] `client_id` is derived by stripping non-digits from `order_token` → collisions

`gateway/tcp_server.h:237-241` builds the client identifier by walking the 14-byte
`order_token` and keeping the digits. Two structurally different tokens (`ABC00001` and
`00001XYZ`) collapse to the same `client_id`. `order_token` is a client-chosen opaque field
in OUCH — nothing constrains it to digits, and nothing in the gateway validates that it is.

**Impact:** cross-client cancel/fill attribution under any client that doesn't happen to
use the tester's zero-padded numeric convention. All four in-repo clients do use it, which
is why this has never fired.

**Fix:** hash the full 14 bytes, or assign the ID server-side from the session and stop
trusting a client-controlled field for identity.

---

## A7. [MED] The UDP order path is an unframed near-duplicate of the TCP decode

`process_message` / `handle_udp_client` (`tcp_server.h:220-305`) is a copy-paste of the TCP
decode block (`:367-392`) minus the framing logic. It accepts orders over UDP, with no
length validation beyond the datagram boundary and no reassembly. Two consequences:

- Every future decode fix (starting with A1) must be made in two places or the paths drift.
- It is a live, unauthenticated order-entry socket that no documentation mentions.

**Fix:** delete it if it's vestigial (nothing in the repo sends orders over UDP), or factor
the shared decode into one function both paths call. Deleting is the smaller diff.

---

## A8. [LOW] Dead code

| Location | What |
| :--- | :--- |
| `tcp_server.h:484-489` | `supported_stocks[4]` — AAPL/MSFT/GOOG/AMZN as `uint64_t` casts. Never referenced. A fossil of the symbol scheme A1 describes. |
| `tester.cpp:32` | `const char* stocks[4] = {"AAPL    ", ...}` — never used; `stock_name` is built at `:63`. |
| `generate_pcap.cpp:20,35` | Same four symbols, actually sent — and therefore 100% rejected (A1). |
| `matching/engine.cpp:60` | `total_processed++`, exposed via `get_total_processed()`, read by nobody. |

Each one is a reader-hours tax: the `supported_stocks` array in particular actively
misleads anyone trying to answer "what symbols does this exchange accept?"

---

## A9. [LOW] Listen fd is level-triggered while client fds are edge-triggered

`tcp_server.h:100-102` registers the listen fd with plain `EPOLLIN`; client and UDP fds use
`EPOLLIN | EPOLLET`. This is *not* a bug — a level-triggered listener with a single
`accept()` per wakeup is correct and arguably safer than the ET equivalent (which requires
an `accept()` drain loop). It is flagged only because `technical-deep-dive.md §5` presents
the gateway as uniformly edge-triggered, and a reader chasing that claim will find a
contradiction that isn't one. One comment on the `epoll_ctl` call closes it.

---

# B. Doc ↔ code discrepancies

## B1. `_mm_lfence()` is claimed in two docs and exists nowhere

`telemetry.md:26-34` and `technical-deep-dive.md:126-134` both present:

```cpp
inline uint64_t get_cycles() {
    uint32_t aux;
    _mm_lfence();
    return __rdtscp(&aux);
}
```

`telemetry.md:26` builds a whole paragraph on it ("we pair the timestamp read with a Load
Fence"). The real function is `core/timer.h`:

```cpp
inline uint64_t get_tsc() {
    unsigned int dummy;
    return __rdtscp(&dummy);
}
```

`grep -rn "_mm_lfence" --include="*.h" --include="*.cpp" .` returns **nothing** across both
projects. Different name, no fence, and no function named `get_cycles` exists.

In fairness to the code: `__rdtscp` already has the load-fence semantics `lfence` would add
(it waits for prior loads to retire) — which is presumably why `rdtscp` was chosen over
`rdtsc`. So the code is right and the docs are wrong, but the docs are wrong in a way that
sounds *more* sophisticated than the truth. Fix the docs to say "`__rdtscp` serialises
against prior loads, so no separate `lfence` is needed" — that's the better answer anyway.

## B2. "Zero-copy" is a `memcpy`

`architecture.md:32` claims `Zero-Copy Cast -> reinterpret_cast<const Order*>(char_buffer)`.
`technical-deep-dive.md:74-77` goes further:

```cpp
// Zero-copy reinterpret
const OuchOrderMessage* msg = reinterpret_cast<const OuchOrderMessage*>(buffer);
```

The actual gateway (`tcp_server.h:225` and `:367`):

```cpp
std::memcpy(&req, buf, sizeof(OuchEnterOrder));
```

That is a copy — 81 bytes of it, per order, on the hot path. Three separate problems here:

- The technique described is not the technique used.
- The type `OuchOrderMessage` does not exist in either codebase (it's `OuchEnterOrder`).
- `technical-deep-dive.md:77` claims the impact is "eliminating … memory copies", which is
  precisely the thing the code does not do.

The `memcpy` is arguably the *right* call — casting a `char*` into a packed struct pointer
is a strict-aliasing and alignment violation, and the compiler will usually elide the copy
into the same loads anyway. So again: correct code, wrong docs. Say "we `memcpy` into a
properly-aligned struct; the compiler elides it — casting would be UB."

## B3. `alignas(64)` in the docs, `alignas(128)` in the code

`technical-deep-dive.md:47-59` is titled "False-Sharing Mitigation (`alignas(64)`)" and shows:

```cpp
alignas(64) std::atomic<size_t> head{0};
alignas(64) std::atomic<size_t> tail{0};
```

`core/lock_free_queue.h:45-52` uses `alignas(128)` on all five members. 128 is the better
choice — Intel's L2 spatial prefetcher pulls in 128-byte-aligned pairs, so 64-byte
separation still false-shares on this exact CPU family. The doc describes the naive fix and
credits the code with it, while the code quietly does the sophisticated one. Update the doc;
this is a point in the author's favour being thrown away.

## B4. The `placement new` example doesn't match the API

`technical-deep-dive.md:15-18`:

```cpp
Order* order = new (pool.allocate()) Order(price, quantity, side);
```

Neither half is real. `MemoryPool::allocate` is variadic and does the placement `new`
*itself* (`memory_pool.h:75-94`), returning a constructed `T*`:

```cpp
T* allocate(Args&&... args) { ... return new (&pool[index]) T(std::forward<Args>(args)...); }
```

And `Order`'s constructor takes six arguments, not three
(`matching/order.h:21`): `Order(internal_id, client_order_id, price, quantity, instrument_id, side)`.
The doc snippet would double-construct and wouldn't compile.

## B5. The `MemoryPool` is described as lock-free; it has a spinlock

`technical-deep-dive.md:15` says "the Gateway requests a pointer from the lock-free
`MemoryPool`". `memory_pool.h:31,76-91` serialises the allocate fast path:

```cpp
std::atomic_flag alloc_lock = ATOMIC_FLAG_INIT;
...
while (alloc_lock.test_and_set(std::memory_order_acquire)) { ... }
```

This spinlock was added deliberately — `known-issues.md:128-132` documents it as the fix for
multiple gateway workers racing on `high_water_mark++`. The doc simply predates the fix. It
matters because a spinlock on the shared allocate path is the most likely contention point
in the whole 4-shard gateway, and the doc tells the reader there isn't one.

## B6. The threading diagram shows 1:1; the design is N×4 fan-in

`architecture.md:76-99` draws Gateway N → SPSC Queue N → Engine N. The actual design (per
`known-issues.md:12-18` and the code) is per-`[shard][gateway-worker]` queues — every
gateway worker has a queue into every engine shard, and each engine drains its row. With 4
gateway threads and `NUM_SHARDS = 4` that's 16 ingress queues, not 4.

The fan-in is the *interesting* part of the design — it's what makes SPSC hold under
`SO_REUSEPORT`, where any worker can receive any instrument. The diagram describes an
architecture that would be broken (and was: it's the `[CRITICAL]` multi-producer bug in
`known-issues.md`).

## B7. `run_sharding.sh` prints "4-Thread Sharding" and runs 1 gateway thread

The script:

```bash
echo "Starting the Exchange Gateway with 4-Thread SO_REUSEPORT Sharding..."
./build/hft_engine/exchange &
```

It never exports `GATEWAY_THREADS`. `app/exchange.cpp:67-68`:

```cpp
const char* env_threads = std::getenv("GATEWAY_THREADS");
int num_gw = env_threads ? std::atoi(env_threads) : 1;
```

Default is **1**. So the script that the README names as the benchmark entry point prints a
claim of 4 and starts 1 — while `README.md:94` reports `Gateway Threads : 4` as sample
output. `hft-trading-firm/src/network/LocalExchangeConnector.h:67` reads the same env var to
size its connection count, so both sides silently fall back to the 1-thread configuration
that `benchmarks.md:75` itself labels **"Saturation (Queueing Delay)"** at 10M msgs/sec.

Anyone who runs the documented command reproduces the *pathological* row of the matrix, not
the optimal one. One line fixes it: `export GATEWAY_THREADS=${GATEWAY_THREADS:-4}`.

## B8. The README's build & run block does not work

`README.md:72-87`, four problems in sixteen lines:

| Line | Claim | Reality |
| :--- | :--- | :--- |
| `cd High-Frequency-Trading-Engine` | repo dir | the tree is `Trading-Ecosystem/` |
| `cmake .. -DCMAKE_BUILD_TYPE=Release` | sets optimisation | `hft_engine/CMakeLists.txt` reads no build-type flags at all (B9) |
| `./scripts/run_sharding.sh` after `cd build` | — | the script uses paths relative to the repo root (`./build/hft_engine/exchange`); run from `build/` it finds nothing |
| `cat new_results.txt` | benchmark results | nothing in the repo ever writes `new_results.txt`; the file does not exist |

## B9. `-O3` is documented; `hft_engine` compiles with no optimisation flags

`benchmarks.md:21` states the compiler config as "GCC / Clang (C++17/20, `-O3` Release Mode)".
The entire `hft_engine/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.16)
project(HFTEngine)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include_directories(src)
add_executable(exchange src/app/exchange.cpp src/matching/engine.cpp)
...
```

No `CMAKE_CXX_FLAGS`, no `CMAKE_CXX_FLAGS_RELEASE`, no `-march=native`, no `-Wall`. With
`-DCMAKE_BUILD_TYPE=Release`, CMake's built-in default gives `-O3 -DNDEBUG` — so the `-O3`
claim survives on a technicality, but *only* because of a default the file never states,
and `-march=native` is absent entirely.

The sibling project knows better — `hft-trading-firm/CMakeLists.txt`:

```cmake
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -flto -DNDEBUG")
```
plus `-Wall -Wextra -Werror -Wpedantic`.

So the two halves of the same ecosystem are built with materially different flags, and the
half making the cycle-count claims is the unoptimised one. Every number in `benchmarks.md`
was measured without `-march=native` or LTO on the engine. Copy the firm's block into
`hft_engine/CMakeLists.txt` and re-measure; `-Werror` will likely also surface the dead code
in A8.

## B10. README "Future Work" lists something already implemented

`README.md:99`: "**NUMA Thread Affinity:** Use `pthread_setaffinity_np` to explicitly pin
Gateway and Matching threads to specific physical CPU cores."

`app/exchange.cpp` already does exactly this — engines pinned to cores 2/4/6/8, publisher to
12, gateway outer thread to 10, OM to 14, via `pthread_setaffinity_np` plus `SCHED_FIFO`
prio 99. `bottlenecks.md:73` makes the same mistake, listing thread affinity as a thing "the
system would require" to scale.

Note the real gap is subtler and is already correctly captured in
`known-issues.md [HIGH]`: the *outer* server thread is pinned, but the worker threads it
spawns inherit nothing. So "affinity" is half-done — but "not started", which is what the
README says, is wrong in the other direction.

## B11. README repo structure omits half of `docs/`

`README.md:56-61` lists five files under `docs/`. The directory also contains
`known-issues.md` and `cpp-prep-for-python-monitoring.md`. Leaving the known-issues file out
of the map, specifically, reads as concealment whether or not it's meant that way.

---

# C. Doc ↔ doc discrepancies

## C1. The timestamp count is three, four, or five depending on which doc you read

| Source | Count |
| :--- | :--- |
| `telemetry.md:42-48` | **three** — "captures cycle counts at three distinct lifecycle boundaries", T0/T1/T2, with a "three-point decomposition" and an ASCII timeline |
| `technical-deep-dive.md:135` | **three** — T0, T1, T2 |
| `benchmarks.md:23` | **four** — "timestamp packets at four distinct lifecycle boundaries" |
| `bottlenecks.md:23` | **four** — same sentence |
| **the code** | **five** — `t1..t4` on the wire in `OuchEnterOrder`, plus a gateway-side t5 |

Three docs, three answers, none of them the code's. `benchmarks.md:29-32` then lists only
*three* attribution stages (TCP / SPSC / Engine) one line after claiming four boundaries —
so `benchmarks.md` contradicts itself internally as well.

This is the single most-asked question about the project ("how did you measure it?"), and
the docs cannot agree on the answer.

## C2. The 299M-cycle figure appears in no benchmark row

`bottlenecks.md:28` — the centrepiece of the flagship debugging story:

> **TCP Network Path:** 299,000,000 CPU cycles (~75 milliseconds)

`benchmarks.md`'s capacity matrix has no such row. The two single-thread saturation
measurements are:

| Load | Threads | TCP Path |
| :--- | :--- | :--- |
| 5,000,000 | 1 | 195,671,018 cycles |
| 10,000,000 | 1 | 242,392,082 cycles |

`bottlenecks.md:18` attributes its 75ms to "5,000,000 msgs/sec on a single thread" — the row
that says 195.7M cycles, ~53% below the quoted figure. `bottlenecks.md:27` also cites "~5,000
CPU cycles" for engine execution against the matrix's 4,997 (close enough), so the engine
number was clearly taken from this table while the TCP number was not.

Relatedly, `benchmarks.md:85` says the 10M/1-thread delay "spikes to over 240,000,000 cycles
(~60ms)" while `bottlenecks.md` calls the *smaller* 5M number 75ms. Both cannot be right.

## C3. The `EBADF` → `SIGBUS` narrative is not internally coherent

`bottlenecks.md:37-52` claims a spin-loop of `read()` calls failing with `EBADF`
"ultimately destabilized the process, culminating in a `SIGBUS` crash."

`EBADF` is a return code. A tight loop of failing `read()`s burns CPU forever; it does not
produce `SIGBUS`. `SIGBUS` is a bus error — an unaligned access, or a touch past the end of
an `mmap`'d region. This codebase has an obvious candidate for a genuine `SIGBUS`: the
`mmap`'d `order_audit.log` (`auxiliary/order_manager.h`), where writing past the mapped
length raises exactly that signal.

So there were most likely **two** bugs, and the doc welds them into one causal chain with
`SIGBUS` as the punchline. As written, the story doesn't survive a follow-up question from
anyone who knows what `SIGBUS` means — which, in the audience for this document, is
everyone. Either split it into two entries or drop the `SIGBUS` claim.

## C4. 4.4 GHz advertised, ~4.0 GHz used in every conversion

`benchmarks.md:16` specifies the CPU as "Intel Core i5-1240P (~4.4 GHz Max Turbo)". Every
cycles→time conversion in the docs implies ~4.0 GHz:

| Claim | Cycles | Stated time | Implied clock |
| :--- | :--- | :--- | :--- |
| `README.md:12`, `benchmarks.md:86` | 1,422,379 | ~355 µs | 4.01 GHz |
| `benchmarks.md:85` | 242,392,082 | ~60 ms | 4.04 GHz |
| `benchmarks.md:51` | ~367 | <100 ns | ≥3.67 GHz |

~4.0 GHz is the honest number for a sustained all-core load on a 1240P (4.4 is a
single-core, short-duration turbo ceiling), so the *conversions* are the trustworthy part.
The spec line is the outlier. Quote the sustained clock, or state the measured TSC frequency
— the code calibrates it at startup, so the real value is available for free and is the only
one that matters for TSC-based timing anyway.

## C5. "Core Business Logic: 350–370 cycles" silently changes subject

`README.md:13` bills "Core Business Logic | 350–370 CPU cycles (<100 ns)" as a headline
result, and `benchmarks.md:91` repeats "core business-logic execution of approximately
350–370 cycles".

That number is from `benchmarks.md:51` and means **Decode + Validation + Enqueue in the
gateway** (82 + 27 + 258 = 367). It is the cost of parsing a message and pushing it onto a
queue.

In the same document, `benchmarks.md:77` gives the matching engine at the optimal operating
point as **4,415 cycles** — 12× larger.

A reader of the README will take "Core Business Logic" to mean the matching engine — the
thing this project exists to build. It doesn't. The engine's own number is a perfectly
respectable 4,415 cycles (~1.1 µs); there's no reason to let the 367 stand in for it. Rename
it "Gateway Ingest Path" and quote the engine separately.

## C6. `bottlenecks.md` engine figure drifts from the matrix

`bottlenecks.md:27` cites "**Trading Engine Execution:** ~5,000 CPU cycles" as the
disproof-of-hypothesis. The matrix's engine numbers range 3,582–5,727 across every load and
thread count. That's fine as a round number — but it's worth noting the engine cycle count
is essentially *flat* across a 10× load range, which is a much stronger version of the claim
`bottlenecks.md` is trying to make and is currently left on the table.

---

# D. Reconciling `known-issues.md`

The prior audit is accurate and its resolutions check out against the tree. Nothing in it
needs retraction. Current status:

| Issue | Severity | Status |
| :--- | :--- | :--- |
| Multiple producers → SPSC queues | CRITICAL | ✅ resolved — per-`[shard][worker]` fan-in queues |
| `SessionManager::next_internal_id` race | CRITICAL | ✅ resolved — atomic `fetch_add`, packed `atomic<uint64_t>` slots |
| `__builtin_clzll(0)` / `ctzll(0)` UB | CRITICAL | ✅ resolved — boundary-safe L2 masks. Independently re-derived during this review: the ladder is safe as written, since L2 bits above `MAX_PRICE` are never set, so `clz`/`ctz` never see zero via that path. **Still has no unit tests.** |
| Gateway workers not pinned / not `SCHED_FIFO` | HIGH | ❌ **open** — see B10; the outer thread is pinned, the workers it spawns are not |
| `internal_id` / `orders_by_id` exhaustion | MED | ✅ resolved — `internal_id` is now the pool slot index; slot 0 reserved as null |
| 1024-fd connection limit | MED | ✅ resolved — per-worker `unordered_map<int, ClientState>` |
| `ClientState` 128 KB × 1024 × threads | MED | ✅ resolved — per-connection, 16 KB, fragment compaction replaces hard reset |
| Audit-log validity only on clean shutdown | MED | ✅ resolved — 64-byte header with atomic `write_index` |
| `dropped_reports` never reported | LOW | ✅ resolved — printed in shutdown stats |
| Publisher dead match-time write | LOW | ✅ resolved — dead `rdtsc` removed from `broadcast` |
| Shutdown race on `Timer` | LOW | ✅ resolved — producers joined before final drain |
| Duplicated protocol header | LOW | ⚠️ mitigated — `static_assert` size guards on both copies; not consolidated |

**What the prior audit missed:** the symbol-encoding mismatch (A1). This is worth calling out
because that audit's verification method is what should have caught it. It certified the
queue fixes with "Verified end-to-end under `GATEWAY_THREADS=4`: audit log contained exactly
1,256,000 events (256k NEW + 744k REJECT + 256k CANCEL)". That run:

- used `tester.cpp`, not the `trading_firm` that `run_sharding.sh` actually launches — so it
  never exercised the `INSTR0..3` path that fails;
- treated 744k rejects as background noise rather than as a signal about the workload (A2).

The queue conclusions are sound — an exact event count under 4 workers does prove the SPSC
fan-in holds. But "verified end-to-end" is doing more work than the evidence supports: the
end-to-end path in the README was never the path that was run.

---

# E. Cross-cutting

**`hft_engine/tests/` is empty.** Not sparse — empty. Zero tests for the order book, the
matching logic, the bitmap ladder, the framing state machine, or the symbol decoder. Three
of the four `[CRITICAL]` findings in `known-issues.md` are exactly what a deterministic unit
test catches on the first run, and A1 — the top finding in this review — is a single
`assert(decode("INSTR0  ") == 0)` away from being impossible to ship.

For a project whose entire thesis is "measure, don't speculate", the absence of a test suite
is the most quotable inconsistency in the repo. The highest-leverage next commit is not a
fix; it's a `test_orderbook.cpp` with ten asserts.

**Two build configurations for one ecosystem** (B9). The firm gets `-O3 -march=native -flto`
and `-Werror`; the engine gets whatever CMake defaults to. Unify them before re-measuring
anything.

---

# Recommended order of work

1. **A1** — make the gateway and the firm agree on a symbol scheme. Until this lands, the
   documented benchmark measures a reject loop.
2. **A2** — reconcile 256 vs 1000 instruments, then re-run and publish the accepted/rejected
   split.
3. **B7 + B8 + B9** — make `run_sharding.sh` export `GATEWAY_THREADS=4`, fix the README's
   build block, give `hft_engine` the firm's compiler flags. Then re-measure. Every number in
   `benchmarks.md` is currently from an unoptimised engine on a 1-thread gateway.
4. **E** — ten asserts on the order book and the symbol decoder.
5. **A3, A4** — count drop-copy losses; stop throwing on the hot path.
6. **B1–B6, B10, B11, C1–C6** — one documentation pass. Most of these make the project look
   *better*, not worse: the code is more sophisticated than the docs describing it in at
   least three places (`alignas(128)`, `__rdtscp`'s implicit fence, the fan-in queue design).
