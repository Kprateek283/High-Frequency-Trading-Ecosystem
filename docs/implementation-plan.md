# Implementation Plan

Derived from [`review-findings.md`](./review-findings.md) (items A1–A9, B1–B11, C1–C6, E),
[`known-issues.md`](./known-issues.md) (one item still open), and
[`cpp-prep-for-python-monitoring.md`](./cpp-prep-for-python-monitoring.md) (§1–§7, of which
§5 is done). No code here — this is the build order and the reasoning behind it.

**Structure:** seven phases, strictly layered. Each phase only depends on the phases above
it, each ends with a verification gate, and nothing in a later phase forces rework of an
earlier one. The C++ engine is stabilised first (Phases 0–3), then the machine-readable
contract Python consumes is built on top of it (Phase 4), then the Python modules
themselves (Phase 5), and documentation is rewritten once — last — against the final code
and the re-measured numbers (Phase 6).

**Why this order and not the review's:** the review's "recommended order of work" ranks by
severity. This plan ranks by *dependency*. The severity-ordered list would fix the symbol
mismatch (A1) first — but that fix has to live in the canonical protocol header, which
doesn't exist yet, and has to be verified by tests, which don't exist yet. Base first.

---

## Dependency map (one screen)

```text
Phase 0  Build flags ── Canonical protocol header ── Test harness
              │                    │                      │
Phase 1  Decode unification → Symbol scheme (A1/A2) → client_id (A6)   [gate: matches > 0]
              │
Phase 2  Pool-exhaustion reject (A4) · Drop-copy counter (A3) · STP (A5)
              │
Phase 3  Config file (§6) → Worker pinning → Scripts/README run path → RE-MEASURE
              │
Phase 4  Multicast opts (§2) · queue/pool accessors → Stats shm region (§4) ← TSC anchor (§3) · READY/heartbeat (§7)
              │
Phase 5  Python monitoring — module names & deps: see dependency.md (authoritative)
              │
Phase 6  Documentation truth pass (B1–B6, B10–B11, C1–C6, README numbers)
```

---

# Phase 0 — Foundation: build, contract, tests

Everything later either compiles under these flags, includes this header, or is verified
by this harness. Nothing here changes runtime behaviour.

### 0.1 Unify the build (review B9, prep §7.1)

- Give `hft_engine/CMakeLists.txt` the same release flags the firm already has
  (`-O3 -march=native -flto -DNDEBUG`) and the same warning set
  (`-Wall -Wextra -Werror -Wpedantic`). One ecosystem, one build configuration.
- Pin `RUNTIME_OUTPUT_DIRECTORY` for both projects to a stable `build/bin/` so every
  script and (later) the Python orchestrator has one known path for binaries.
- Expect `-Werror` to flag the dead code from A8 (`supported_stocks[4]`, tester's unused
  `stocks[4]`, `get_total_processed()`). Delete it here rather than suppress — this phase
  is where the tree gets clean enough to build on.

**Why first:** every cycle number ever published came from an engine built without these
flags. Any fix verified before this lands gets verified against the wrong binary.

### 0.2 Canonical wire schema (prep §1, known-issues "duplicated protocol header")

- Make `hft_engine/src/protocol/messages.h` the single source of truth. The firm's
  `src/network/messages.h` becomes an include of it (or is deleted and the firm's include
  paths pointed at the engine's header). The existing `static_assert` size guards move
  with it — one copy, not two.
- Add a `PROTOCOL_VERSION` constant now; Phase 4 publishes it in the stats region and
  Phase 5's Python asserts against it.
- This header is also where the Phase 1 symbol table will live — that's the reason this
  must precede A1, not follow it.

### 0.3 Test harness (review E)

- Stand up `hft_engine/tests/` with a plain-assert test binary wired into CTest. No
  framework — `assert` and `main` are enough for deterministic unit tests of
  single-threaded logic.
- First tests *lock in current behaviour* before anything changes: bitmap ladder
  boundary cases (empty book, word boundary, L2 group boundary — the exact cases from the
  resolved CRITICAL), SPSC queue push/pop/full/empty, framing compaction on split
  messages, wire-struct sizes.
- Every subsequent phase adds its tests to this target. The gate for each phase is
  "harness green", so it has to exist before the first fix.

**Gate for Phase 0:** both projects build clean under `-Werror` from one protocol header;
`ctest` runs and passes; binaries land in `build/bin/`.

---

# Phase 1 — Protocol and decode correctness (the A1 cluster)

The single most important functional change: after this phase, the documented benchmark
actually matches orders. All four items touch the same ~60 lines of gateway decode, which
is why they are one phase and land in this order.

### 1.1 One decode path (review A7)

- Delete the UDP order-entry path (`process_message` / `handle_udp_client` and the
  9092 sockets). Verified: nothing in the repo sends orders over UDP — `replay`,
  `tester`, `market_maker`, and the firm all use TCP 9091. (Market-data multicast is a
  different socket and untouched.)
- What remains is a single TCP decode function. Do this *before* the symbol fix so A1 is
  implemented once, not twice in near-duplicate blocks that will drift.

### 1.2 Canonical symbol scheme (review A1 + A2) — **decision required, recommendation below**

- Define one symbol→instrument mapping in the Phase-0 protocol header, next to the wire
  structs, so client and gateway physically cannot disagree: the table (or the encode/
  decode pair) is compiled into both sides.
- **Recommendation:** keep `STK#####` as the canonical wire encoding (it's the only
  scheme with a trivial, branch-light decode) and change the *clients* to emit it:
  the firm's `INSTRUMENT_STRINGS`, `market_maker`'s `AAPL`, and `generate_pcap`'s four
  symbols all become `STK00000..` The alternative — teaching the gateway four schemes —
  spreads the mapping across the hot decode path forever.
- **A2 decision:** reconcile 256 vs 1000. Recommendation: keep `MAX_INSTRUMENTS = 256`
  and drop `tester.cpp`'s `num_symbols` to 256. Raising the cap to 1024 quadruples
  per-shard book memory (price-ladder bitmaps are per instrument) for no benchmark
  benefit; a 256-symbol workload already exercises every book path. Whichever way it
  goes, the benchmark write-up must state the cap and publish the accepted/rejected
  split (Phase 3.5 / Phase 6).
- The invalid-symbol sentinel (currently a magic `999`) becomes a named constant in the
  header, and the risk engine's check references `MAX_INSTRUMENTS` from the same header —
  one place defines validity.

### 1.3 Server-side client identity (review A6)

- Stop deriving `client_id` by stripping digits from the client-controlled
  `order_token`. Assign identity from the session (the connection) at the gateway.
  `order_token` stays what OUCH says it is: an opaque client token echoed back, not an
  identity source. This lands here because it changes the same decode function 1.1 just
  unified.

### 1.4 Tests for the cluster

- Unit: decode asserts for every historical client encoding — `STK00000` maps to 0,
  `STK00255` to 255, out-of-range and garbage map to the invalid sentinel and are
  rejected. The review's one-liner (`assert(decode("INSTR0  ") == …)`) becomes real, with
  whatever verdict 1.2 decided.
- End-to-end: this phase's acceptance test is the documented run itself —
  `run_sharding.sh` (still 1-thread until Phase 3, that's fine) must produce
  **matches > 0** and an exact audit-log event count.

**Gate for Phase 1:** unit tests green; the firm's traffic produces fills; audit log
shows NEW/EXEC/CANCEL with a reject rate that is a property of the workload, not of a
parsing failure.

---

# Phase 2 — Engine robustness

Independent of each other, all dependent on Phase 1 (they touch the reject flow and the
stats that Phase 1 stabilised). Ordered by blast radius.

### 2.1 Pool exhaustion → reject, not `std::terminate` (review A4)

- `MemoryPool::allocate` returns null on exhaustion instead of throwing; the gateway
  treats it exactly like a risk reject (the flow Phase 1 just consolidated): reject the
  order, count it, keep the session alive. The engine keeps running when one shard's
  pool fills — exhaustion is a client behaviour, not a process fault.
- Test: fill a small pool, verify orders reject and the process survives; verify the
  audit log records the rejects.

### 2.2 Count drop-copy loss (review A3)

- Mirror `broadcast`: a failed drop-copy push increments a `dropped_drop_copies`
  counter, printed in shutdown stats next to `dropped_reports`. Two lines, but placed
  here deliberately: this counter is also a Phase 4 stats-region field, so it must exist
  before the region schema is frozen.
- Test: force a full drop-copy queue in a unit test and assert the counter moves.

### 2.3 Self-trade prevention (review A5) — **decision required, recommendation below**

- **Recommendation:** implement minimal STP — if the incoming order would execute
  against a resting order from the same client identity (from 1.3), reject the incoming
  order (cancel-newest). Smallest correct policy, no book surgery, one comparison on the
  match path. The in-repo `market_maker` quotes both sides of one instrument, so the
  demo itself trips this without STP.
- If instead it's declared out of scope, that's acceptable — but it must be stated in
  the README (Phase 6 picks this up either way).
- Test: same-client cross rejects; different-client cross fills.

**Gate for Phase 2:** exhaustion stress run survives; audit counts stay exact; STP test
green (or scope decision recorded).

---

# Phase 3 — Configuration, pinning, and honest benchmarks

This phase exists to make one statement true: *the documented command reproduces the
documented numbers.* Config is pulled forward from the prep doc's §6 because two things
in this phase (worker pinning, the run script) and everything in Phases 4–5 read it.

### 3.1 Externalize configuration (prep §6) — **format decision, recommendation below**

- One config source read by the engine, the firm, the scripts, and later the Python
  orchestrator. Minimum keys: TCP/UDP ports, multicast group/port, shard count, gateway
  thread count, CPU core map, stats-shm path, audit-log path.
- **Recommendation: a plain env file** (`config.env`, `KEY=value`), not YAML/TOML. Every
  consumer already reads env (`GATEWAY_THREADS` works this way today); bash sources it,
  C++ `getenv`s it, Python `os.environ`s it. No parser, no new dependency, one file.
  Defaults live in code; the file overrides.

### 3.2 Pin the gateway workers (known-issues [HIGH] — the last open item)

- Apply CPU affinity and `SCHED_FIFO` inside each worker's loop entry, with per-worker
  cores from the 3.1 core map. This closes the only unresolved finding in
  `known-issues.md`: today the outer server thread is pinned but the workers it spawns
  float at normal priority.
- Sequenced *after* 3.1 so the core map is written once (config), not hardcoded twice.
- Sequenced *before* 3.5 because pinning changes tail latency — numbers measured before
  this land are stale the day it merges.

### 3.3 Fix the run path (review B7)

- `run_sharding.sh`: source the config, default `GATEWAY_THREADS` to 4, resolve paths
  from the repo root regardless of invocation directory, and write results to the file
  the README tells users to read (or stop telling them). The script's banner must print
  the thread count it actually starts.

### 3.4 Fix the README build/run block (review B8)

- Correct the clone directory name, run the script from the repo root, and make the
  "view results" step point at a file the run actually produces. (Prose claims about
  numbers are Phase 6; this item is only "the commands work".)

### 3.5 Re-measure everything

- Re-run the capacity matrix with: real compiler flags (0.1), a workload that matches
  (1.2), pinned workers (3.2), and the 4-thread default (3.3). Record per row: TCP path,
  engine cycles, end-to-end, **and the accepted/rejected split** (A2's obligation).
  Capture the measured TSC frequency from the startup calibration — it replaces the
  4.4 GHz spec-sheet number in every cycles→time conversion (review C4).
- Store raw results in-repo (the results file the script writes). These numbers are the
  *input* to Phase 6; no doc is edited until they exist.

**Gate for Phase 3:** a fresh clone, following the README verbatim, produces a results
file with matches > 0 on a 4-thread gateway. That sentence is currently false in four
independent ways; this phase makes it true.

---

# Phase 4 — The monitoring contract (C++ side of the Python work)

Prep-doc items, in the prep doc's own order (§2 → §3 → §4 → §7; §5 is already done, §1
landed in Phase 0, §6 in Phase 3). Principle preserved throughout: nothing here touches
the matching hot path — relaxed atomic stores and one-time socket setup only.

### 4.1 Multicast hardening (prep §2)

- Set `IP_MULTICAST_TTL`, `IP_MULTICAST_LOOP`, `IP_MULTICAST_IF` explicitly in the
  Publisher's one-time setup, group/port from the 3.1 config. Today's behaviour survives
  on Linux defaults; Python subscribing from another host/interface is what breaks.

### 4.2 Expose the internals the stats region needs

- `LockFreeQueue` gains a `size()` (derivable from head/tail, relaxed loads — currently
  only `empty()` exists); `MemoryPool` exposes its high-water mark read-only. Pure
  accessors, no behaviour change. Split out as its own step because the region schema
  (4.3) can't be written without them.

### 4.3 Shared-memory stats/health region (prep §4) — the one M-effort item

- A versioned, seqlock-protected struct in `/dev/shm` (path from config): per-shard
  counters (orders in, fills, cancels, rejects), queue depths, pool high-water,
  gateway cycle attribution, `dropped_reports`, and the `dropped_drop_copies` counter
  from 2.2. Writers use relaxed atomic stores; the seqlock gives Python a consistent
  snapshot with retry.
- This deprecates stderr-scraping as the only observability. The shutdown stats print
  stays (it's free), but every number in it must also be in the region.

### 4.4 TSC↔wall-clock anchor (prep §3)

- At startup, store one `(tsc, CLOCK_REALTIME ns, cycles_per_ns)` anchor in the region.
  Python converts any wire TSC to epoch time with arithmetic — no clock syscalls per
  message. Documented assumption: invariant TSC. Lands with 4.3 because the anchor lives
  inside the region struct; listed separately because it's the piece Phase 5's `tsc.py`
  unit-tests against.

### 4.5 Liveness metadata (prep §7)

- PID file on startup; a single `READY` line on stdout once sockets are bound and
  threads pinned (the orchestrator's launch barrier); heartbeat TSC in the stats region
  updated ~100 ms from the existing OrderManager loop. Last in the phase because the
  heartbeat field needs 4.3's region and the paths need 3.1's config.

**Gate for Phase 4:** during a live run, a throwaway reader (a few lines, any language)
mmaps the region and observes monotonically rising counters, a fresh heartbeat, and a
sane anchor; the Publisher's feed is received on an explicitly-joined socket. The
contract Python will consume is now frozen: region layout, audit-log header (§5, done),
ITCH structs, config keys, READY/PID.

---

# Phase 5 — Python monitoring modules

> **Module names and dependency edges are specified in [`dependency.md`](./dependency.md), which is
> authoritative for this phase.** It is finer-grained and wider in scope than the sketch below: `tsc.py` →
> `clock.py`, `itch_client.py` → `feeds/multicast.py`, `book.py` → `core/orderbook.py`, and the flat
> `monitoring/` layout is nested into a package. This phase covers dependency.md's **tiers 0–1 plus
> `orderbook`, `metrics`, `health`, `orchestrator`, and `tui`**; its backtest/replay tiers are separate work.
> The layering rationale below still holds — read it, then take the names from `dependency.md`.

All-new code under `monitoring/`. Layered exactly like the C++ side: schema first,
readers on the schema, derived state on the readers, orchestration and UI on top. Each
layer ships with its smallest meaningful test. Dependencies: stdlib for everything
except the TUI/plots (`rich` or `textual`, and the already-used `matplotlib`) — the
readers must not require third-party packages.

### 5.0 Schema layer — `wire.py`, `tsc.py`

- `wire.py`: struct definitions mirroring the canonical header — ITCH message, audit-log
  header + `OrderLogEntry` (including its `alignas(32)` padding), stats-region layout,
  `PROTOCOL_VERSION`. The unit test asserts every `struct.calcsize()` equals the sizes
  the C++ `static_assert`s pin — the same numbers, verified from both languages.
  This is the *fourth* copy of the schema the prep doc warned about; the calcsize-test
  plus the version constant is what keeps it honest.
- `tsc.py`: TSC→epoch-ns conversion from the §3 anchor. Test with a synthetic anchor.

### 5.1 Reader layer — one module per feed, no cross-dependencies

- `stats_reader.py`: mmap the shm region read-only, seqlock retry loop, return a
  plain snapshot object. Test against a fixture file written with `wire.py` itself.
- `audit_reader.py`: mmap `order_audit.log`, validate magic/version/entry-size,
  acquire-read `write_index`, yield entries; supports live tailing (re-read the index)
  and post-crash replay identically — the §5 design's whole point.
- `itch_client.py`: join the multicast group (same socket pattern as the firm's
  listener: `SO_REUSEADDR`, bind `INADDR_ANY`, `IP_ADD_MEMBERSHIP`), decode datagram
  batches with `wire.py`.
- All three take their paths/addresses from the 3.1 config. None imports another.

### 5.2 Derived-state layer

- `book.py`: reconstruct per-instrument books from the ITCH stream. Must implement the
  recycled-id semantics the prep doc flags: order-reference ids are unique per
  `(shard, id)` *while resting* and are reused after an order leaves the book — an `A`
  (add) always starts a fresh order for that id. Test: a scripted event sequence with a
  deliberate id reuse.
- `metrics.py`: rates and deltas from successive stats snapshots (orders/sec, fill
  ratio, queue-depth trends, pool headroom, drop counters), heartbeat-age computation
  via `tsc.py`. Pure functions over snapshots — trivially testable.

### 5.3 Orchestration layer — `orchestrator.py`

- Launch exchange (and optionally firm/tester) from config via `subprocess`; wait on the
  `READY` line with a timeout; supervise via PID + heartbeat age; deliver SIGINT and
  verify clean shutdown; collect exit stats. This is what replaces `run_sharding.sh`'s
  `sleep 2` with an actual readiness barrier — the shell script stays as the thin
  no-Python path, the orchestrator becomes the reliable one.
- Test: launch against a real engine build in CI-local mode — start, READY, heartbeat
  fresh, shutdown clean.

### 5.4 Presentation layer — TUI and plots

- `tui.py`: live dashboard over 5.1/5.2 — per-shard health panel (stats region), trade
  tape (audit tailer), top-of-book (book rebuild), engine liveness (heartbeat). This is
  the first consumer of everything below it and the reason the layering exists.
- Rewrite `scripts/plot.py` to read the Phase-3.5 results file instead of its current
  hardcoded arrays (today it silently embeds the same numbers the docs disagree on —
  one more copy of unverifiable data). Plot generation becomes reproducible from data.

**Gate for Phase 5:** with the engine under load, the TUI shows live counters, fills on
the tape, and a book that matches a spot-check against the audit log; `pytest` (or plain
asserts) green across 5.0–5.3.

---

# Phase 6 — Documentation truth pass

Last, deliberately: every doc statement is written against the final code and the
Phase-3.5 measurements, so this pass happens exactly once. One PR, mechanical, using
`review-findings.md` B and C sections as the checklist:

- **B1** telemetry docs: drop the fictional `_mm_lfence` snippet, show the real
  `get_tsc()`, and state the better truth — `__rdtscp` already orders against prior
  loads.
- **B2** replace "zero-copy cast" with the real (and more defensible) `memcpy`-then-elide
  explanation; fix the nonexistent type name.
- **B3** `alignas(128)` and *why* (L2 spatial prefetcher pairs), not `alignas(64)`.
- **B4** placement-new example rewritten to match the actual variadic `allocate()` API.
- **B5** MemoryPool described as "lock-free deallocate, spinlocked allocate" — matching
  what known-issues documented as deliberate.
- **B6** threading diagram redrawn as the N×M fan-in it actually is.
- **B10** Future-Work list updated: thread affinity is done (Phase 3.2 finished it), not
  future.
- **B11** repo map lists all of `docs/`, including `known-issues.md`,
  `review-findings.md`, and this file.
- **C1** one timestamp story everywhere, matching the code's actual probe count.
- **C2/C6** every cycle figure in `bottlenecks.md` traced to a row of the *new* matrix;
  the 299M orphan replaced or annotated as a pre-fix historical number.
- **C3** the EBADF/SIGBUS narrative split into its two real bugs (spin-loop; mmap
  overrun) or the SIGBUS claim dropped.
- **C4** all cycles→time conversions use the measured TSC frequency captured in 3.5.
- **C5** README headline renamed to "gateway ingest path", with the matching engine
  quoted as its own honest number.
- **A9** one comment in the code (level-triggered listener is intentional) plus one
  sentence in the deep-dive, closing the false contradiction.
- README key-results table and sample output replaced wholesale with Phase-3.5 numbers.
- `known-issues.md` gets a final status note (worker pinning resolved in Phase 3.2);
  `review-findings.md` items get resolution annotations the way known-issues did it.

**Gate for Phase 6:** grep-level check that no doc quotes a number absent from the
results file, no doc shows an API that doesn't compile, and the three docs answer the
timestamp question identically.

---

# Decisions needed before the phase that consumes them

| # | Decision | Consumed by | Recommendation |
| :- | :--- | :--- | :--- |
| 1 | Canonical symbol encoding | 1.2 | Keep `STK#####` on the wire; change all clients to emit it |
| 2 | 256 vs 1000 instruments | 1.2 | Keep `MAX_INSTRUMENTS=256`; tester sends 256 symbols; publish the split |
| 3 | Self-trade prevention in or out of scope | 2.3 | In — minimal cancel-newest (reject incoming on same-client cross) |
| 4 | UDP order-entry path | 1.1 | Delete (confirmed: no in-repo client uses it) |
| 5 | Config format | 3.1 | Plain env file; no parser, all three languages read it natively |
| 6 | TUI dependency | 5.4 | `rich` (already ubiquitous); readers stay stdlib-only regardless |

# What is deliberately NOT in this plan

- Rewriting the matching engine, order book, or queue internals — all resolved items in
  `known-issues.md` stay as-is; the plan builds on them, it doesn't reopen them.
- Kernel bypass (DPDK/ef_vi), MPSC queues, dynamic shard counts — future work, unchanged.
- A second benchmark campaign beyond 3.5 — one honest matrix is the deliverable; tuning
  for better numbers is a separate project with this plan as its prerequisite.
