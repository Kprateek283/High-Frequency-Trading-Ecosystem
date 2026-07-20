# Agent Handoff Brief

Instructions for an implementing agent starting cold on this repo. Read this file first,
then the documents below in order. This brief adds only what the other docs don't carry:
locked decisions, repo orientation, invariants, working commands, and stop conditions.
It deliberately does **not** restate the work items — the plan owns those.

## Mission

Execute [`implementation-plan.md`](./implementation-plan.md) phase by phase, in order.
The repo is an HFT exchange simulator (C++20 engine + C++ trading-firm client) with a
planned Python monitoring layer. The engine's concurrency bugs are already fixed
(`known-issues.md`); your job is the remaining correctness, benchmark-honesty,
monitoring-contract, and Python work.

## Reading order

1. **[`implementation-plan.md`](./implementation-plan.md)** — the work order. Phases 0–6,
   each with a verification gate. This is what you execute, top to bottom.
2. **[`review-findings.md`](./review-findings.md)** — the *why*. The plan cites its items
   as A1–A9 (code), B1–B11 (doc↔code), C1–C6 (doc↔doc), E (cross-cutting). Read A and E
   fully; skim B/C until Phase 6.
3. **[`dependency.md`](./dependency.md)** — authoritative for Phase 5: Python module
   names, package layout, tier graph, and the C++↔Python interface contracts (I1–I8).
   Its blocker notes (I3/I6) explain what your Phase 1 work unblocks.
4. **[`cpp-prep-for-python-monitoring.md`](./cpp-prep-for-python-monitoring.md)** — the
   §1–§7 detail specs the plan's Phase 4 references. §5 (audit-log header) is already
   implemented; treat its layout block as the frozen contract.
5. **[`known-issues.md`](./known-issues.md)** — history. Everything marked ✅ RESOLVED is
   settled; do not reopen or "improve" those fixes. The one open [HIGH] (gateway workers
   unpinned) is plan item 3.2.

Line numbers in all docs drift as you edit — locate code by grep/symbol, never by the
cited line.

## Locked decisions — do not re-litigate

The plan's decision table is resolved. All six recommendations are adopted:

| # | Decision |
| :- | :--- |
| 1 | Canonical symbol encoding is `STK#####` (`STK` + 5 ASCII digits, 8 bytes). All clients change to emit it; the gateway decodes only it. |
| 2 | `MAX_INSTRUMENTS` stays **256**. `tester.cpp` drops to 256 symbols. The benchmark write-up states the cap and the accepted/rejected split. |
| 3 | Self-trade prevention is **in scope**: reject the incoming order on a same-client cross (cancel-newest). |
| 4 | The UDP order-entry path (port 9092) is **deleted**, not refactored. Market-data multicast is a different socket — untouched. |
| 5 | Config is a **plain env file** (`config.env`, `KEY=value`). Defaults in code; file overrides; bash sources it, C++ `getenv`s, Python reads `os.environ`. |
| 6 | TUI uses **`rich`**. Everything below the TUI (tiers 0–4 in `dependency.md`) is stdlib-only. |

## Architecture in one paragraph

Clients (`hft-trading-firm/trading_firm`, or `hft_engine` tools `tester`/`replay`/
`market_maker`) send binary OUCH orders over TCP :9091. Gateway worker threads
(`SO_REUSEPORT` + `epoll`, count from `GATEWAY_THREADS`, default 1) decode, run pre-trade
risk, allocate an `Order` from a per-shard `MemoryPool`, and push an `EngineTask` onto a
per-`[shard][worker]` SPSC queue. Four matching-engine shards (instrument % 4) each drain
their row of queues and match price-time-priority against a bitmap-ladder order book.
Results fan out on more SPSC queues to: a `Publisher` (ITCH multicast `239.255.0.1:12345`
via `sendmmsg`) and an `OrderManager` (drop-copy → mmap'd `order_audit.log` with a live
`write_index` header). Latency probes `t1..t4` ride inside `OuchEnterOrder`; all
timestamps are raw `__rdtscp` cycle counts.

## File map (what you'll actually touch)

```text
hft_engine/src/
  protocol/messages.h        wire structs + static_asserts → becomes THE canonical header (0.2) + symbol scheme (1.2)
  gateway/tcp_server.h       epoll loops, decode, framing → UDP-path delete (1.1), symbol fix (1.2), client_id (1.3), pinning (3.2)
  gateway/risk_engine.h      pre-trade checks, MAX_INSTRUMENTS cap (1.2)
  gateway/session_manager.h  client→internal id map (packed atomics) (1.3)
  core/memory_pool.h         mmap pool, spinlocked allocate → null-on-exhaustion (2.1), high-water accessor (4.2)
  core/lock_free_queue.h     SPSC ring → size() accessor (4.2)
  core/timer.h               get_tsc(), TSC calibration → anchor source (4.4)
  matching/orderbook.h       book, bitmap ladder, broadcast/send_drop_copy → drop counter (2.2), STP (2.3)
  matching/engine.{h,cpp}    shard loop, cancel validation
  market_data/publisher.h    ITCH multicast → socket options (4.1)
  auxiliary/order_manager.h  audit log + header (done, §5) → heartbeat writer (4.5)
  app/exchange.cpp           main: threads, pinning, shutdown → config (3.1), READY/PID (4.5)
  tools/{tester,market_maker,generate_pcap,replay}.cpp   clients → symbol change (1.2)
hft_engine/tests/            EMPTY → harness goes here (0.3)
hft-trading-firm/src/network/{messages.h, LocalExchangeConnector.h}   duplicate header (0.2), INSTR0-3 symbols (1.2)
scripts/run_sharding.sh      the documented benchmark entry → fix (3.3)
scripts/plot.py              hardcoded numbers → read results file (5.4)
CMakeLists.txt (root) · hft_engine/CMakeLists.txt (no flags — fix 0.1) · hft-trading-firm/CMakeLists.txt (reference flags)
monitoring/                  does not exist → Phase 5 creates it (layout per dependency.md §3)
```

## Invariants — violate none of these

- **SPSC discipline.** Every `LockFreeQueue` has exactly one producer thread and one
  consumer thread. If a change adds a producer, it adds a queue and the consumer fans in.
  This rule was the source of two past CRITICALs; it is load-bearing.
- **Hot path purity** (decode → risk → queue → match → broadcast): no heap allocation,
  no exceptions, no syscalls, no new locks. The pool's allocate-spinlock is the one
  existing, deliberate exception. Monitoring writes are relaxed atomic stores only.
- **Wire layout is versioned.** Any change to a `#pragma pack` struct requires bumping
  `PROTOCOL_VERSION`, updating the `static_assert`s, and (once it exists) `wire.py` — in
  the same commit. Note `OrderLogEntry` is *not* packed (`alignas(32)`, padding is part
  of the on-disk audit format).
- **`internal_id` is the pool slot index.** Slot 0 is the reserved null handle. IDs
  recycle when orders leave the book; stale cancels are caught by validating
  `client_order_id` in the engine. Python's book rebuild must treat every ITCH `A` as a
  fresh order for that id (`dependency.md`, decoder note).
- **The audit-log header layout** in prep-doc §5 is frozen — readers depend on the
  magic/version/entry_size/write_index offsets exactly as written there.

## Build & run (verified at v1.0.0)

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release      # from repo root; builds both projects
cmake --build build -j
(cd build && ctest)                            # C++ suite

python3 -m monitoring.run_tests                # Python suite; runs with nothing installed

# The TUI needs `rich`. Debian/Ubuntu refuse system-wide pip installs (PEP 668),
# so use a venv rather than `pip install -r` against the system interpreter.
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt

./scripts/run_sharding.sh                      # the documented benchmark; run from repo root

GATEWAY_THREADS=4 ./build/bin/exchange         # terminal 1 (writes order_audit.log to cwd)
./build/bin/tester                             # terminal 2 — env: TARGET_RATE (msgs/s), WORKLOAD_TYPE (1-4)
.venv/bin/python -m monitoring.tui.app         # terminal 3 — live dashboard
```

Cautions: `SCHED_FIFO`/`mlockall`/huge pages need privileges the engine often does not
have; it falls back with stderr warnings, so read stderr on the first run. Engine threads
busy-spin at `SCHED_FIFO` 99 pinned to the cores in `config.env`: on a desktop without
`scripts/setup_isolcpus.sh` applied, expect the machine to be sluggish under load, and
expect latency measurements from such a box to be meaningless.

The tester is a single client, and self-trade prevention rejects same-client crosses, so
`tester` alone produces **no fills** by design. Use `./build/bin/liquidity` (two
connections, deterministic 10,000 matches) when you need fills to look at.

## Working rules

- One phase at a time; a phase's gate must pass before the next phase starts.
- Commit per numbered plan item, message prefixed `phase-N.M:`. Never commit a red
  harness.
- Verify by running, not by reading: gates are commands and observable outputs, not code
  review. The Phase-1 gate is literally "the firm's traffic produces fills".
- If code contradicts these docs, trust the code, note the discrepancy in your report,
  and continue — do not silently "fix" the doc mid-phase (Phase 6 is the doc pass).

## Stop conditions

- **Phase 3.5 (re-measurement) and the numeric parts of Phase 6** need this machine idle
  enough to run a 10M msg/s load meaningfully. If it isn't (or you cannot judge), do
  3.1–3.4, skip 3.5, continue with Phases 4–5, do the non-numeric Phase-6 items, and
  leave `TODO(measure)` markers wherever a number would go. Say so in your report.
- Phase 5's scope is `dependency.md` tiers 0–1 plus `core/orderbook`, `core/metrics`,
  `health`, `orchestrator`, `tui` — exactly what the plan's Phase-5 note says. The
  backtest/replay/signals tiers are **out of scope**; do not start them.
- Anything blocked on an unwritten spec (I7 control socket, gap-detection sequence) is
  out of scope — flag it, don't invent it.

## Definition of done — **all gates met at v1.0.0**

The plan is complete and tagged. Kept as a record of what each gate required.

- [x] **P0** both projects build clean under `-Werror` from one protocol header; `ctest` passes; binaries in `build/bin/`
- [x] **P1** decode unit tests green; firm traffic produces **fills**; exact audit-log event count
- [x] **P2** exhaustion stress survives (no terminate); drop-copy counter surfaces; STP test green
- [x] **P3** fresh clone + README verbatim → results file with matches > 0 on a 4-thread gateway
- [x] **P4** external reader sees rising counters, fresh heartbeat, sane TSC anchor; multicast received on an explicitly-joined socket
- [x] **P5** TUI shows live shard stats, trade tape, top-of-book against a running engine; Python tests green
- [x] **P6** no doc quotes a number absent from the results file; no doc shows an API that doesn't compile; all docs agree on the timestamp count

One gate was met only in part, deliberately. **P3's re-measurement** covers gateway ingest
throughput (`benchmark_results.txt`, reproducible via `scripts/measure_throughput.py`) but
**not** end-to-end latency percentiles: this box cannot grant `SCHED_FIFO`, and tail latency
measured under arbitrary preemption describes the scheduler rather than the engine. That
remains `TODO(measure)` pending different hardware.

**Current status lives in [`v1.0.0-defects.md`](./v1.0.0-defects.md)**, not here — this file
is the original brief for executing the plan, and the plan is done.
