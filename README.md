# High-Frequency Trading Ecosystem

## 1. Project Overview
A cycle-accurate High-Frequency Trading (HFT) ecosystem built in modern C++ to explore the performance limits of POSIX userspace networking without kernel-bypass technologies such as DPDK.

The project models both sides of a trading venue: a Trading Firm Simulator and an Exchange comprising a sharded TCP Gateway, lock-free ingestion pipeline, Pre-Trade Risk Engine, and deterministic Price-Time Priority Matching Engine.

## 2. Key Results
| Metric | Result |
| :--- | :--- |
| **Functional run** | 4-thread gateway matches orders end-to-end: 10,000 orders → 20,000 fills, **0 rejects** (`results.txt`) |
| **Gateway Architecture** | 4-thread `SO_REUSEPORT` sharding |
| **Latency probes** | 5-point `__rdtscp` decomposition (4 on-wire + gateway ingress) |
| **Gateway ingest** | **2.06M orders/sec** (median of 9 runs, spread 19.4%) at 4 workers × 4 concurrent clients; scales 481k → 1.09M → 2.06M as clients are added (`benchmark_results.txt`) |
| **Gateway Ingest Path** | `epoll_wait` **41**, `read` **41**, Decode **95**, Validate **17**, Enqueue **228** (Allocate **80**, Record **19**, Push **84**), Egress drain **12** — **433 cycles/order** total, median of 9 runs (ingest path only, *not* the matching engine). These are *elapsed* cycles from `__rdtscp`, so they include stalls and contention — see [`benchmarks.md`](./docs/benchmarks.md) |
| **End-to-End Latency** | `TODO(measure)` — `SCHED_FIFO` is now granted here and made things *worse* ([`docs/scheduling.md`](./docs/scheduling.md)); the blocker is `isolcpus`, not privilege |

> **Read the throughput number with its caveat.** It was measured by
> [`scripts/measure_throughput.py`](./scripts/measure_throughput.py), which samples
> `engine_orders_in` from the stats region rather than trusting a client's send rate
> (a `send()` returns once buffered, so client-side "throughput" is offered load, not
> work done). The counter matters: an earlier version of this benchmark read
> `orders_in`, which is incremented by the OrderManager **three hops downstream** of
> the engine and silently undercounts whenever the drop-copy queue overflows. It was
> measuring the audit logger, not the exchange. `engine_orders_in` is incremented by
> the matching engine itself, on its own cache line, per shard.
>
> **This ceiling is the load generator, not the exchange.** At the 2.06M operating
> point the gateway is **~87% idle** and the engine shards are **~96% idle** — both
> sit in their empty-poll branches waiting for work. Firm and exchange share one
> laptop; the exchange is pinned to the P-cores (`EXCHANGE_CPUSET`, default `0-10`)
> and the `tester` processes to the E-cores (`TESTER_CPUSET`, default `11-15`), so
> the generators cannot produce TCP traffic fast enough to saturate a gateway that
> costs ~433 cycles/order. Ingest was still climbing at four clients — it has not
> saturated, and the number is a floor for this box, not a ceiling for the design.
>
> **The jump from the previously published 1.68M is a harness fix, not an engine
> improvement.** The load generators used to run unpinned and competed with the
> exchange for the very P-cores under measurement; pinning them off those cores is
> worth +31% at four clients on its own and cut the 4-client spread from 52.7% to
> 19.4%. The same fix made the 1- and 2-client points ~9% *worse* (533k → 481k,
> 1.19M → 1.09M) with wider spreads, because at one or two clients there is no
> parallelism to offset running the generators on 3300 MHz E-cores. Full attribution,
> including the arm that isolates the core map, is in `benchmark_results.txt`.
>
> **Per-order cycle counts used to be bimodal, and no longer are.** Under the old
> harness, `Total/Order` across nine runs split into two clusters — five at 742–1056
> and three at 1601–1861 — and that was attributed to gateway workers landing on SMT
> siblings ([`docs/bottlenecks.md`](./docs/bottlenecks.md) §10). Nine fresh runs
> measure 398–500, unimodal, median 433. The split is gone, and so is roughly half
> the absolute cost. Two things changed at once — the load generators moved off the
> measured cores, and the core map was corrected — so **which one dissolved the
> bimodality is not attributed**, though generator contention is the likelier
> candidate given `__rdtscp` counts elapsed cycles rather than retired instructions.
> The recorded trade-off from that analysis (per-order efficiency and wall-clock
> throughput move in opposite directions, so the SMT-inclusive layout — now `0-10` —
> stays published) is unaffected: both of its arms were measured the same way.
>
> **Latency percentiles are deliberately still unmeasured, and `SCHED_FIFO` did not
> fix that.** The privilege is now granted (`ulimit -r` is 80, and the exchange
> verifies per run that it got it: `RT_SCHED: granted=11/11 priority=80`). Realtime
> scheduling turned out to be **59% slower** at one client, because `SCHED_FIFO` never
> preempts a same-priority peer and the engine busy-spins — four shards hold four CPUs
> permanently while eleven realtime threads contend for eight logical ones. RT
> throttling then adds a ~50 ms deschedule every second, which alone would dominate any
> p99.9. The full comparison and the three mechanisms behind it are in
> [`docs/scheduling.md`](./docs/scheduling.md); the machine itself is described in
> [`docs/machine-profile.md`](./docs/machine-profile.md). The real blocker is
> `isolcpus`, not privilege. Publishing a tail from this box would repeat the mistake
> this project already corrected once.
>
> Earlier headline numbers were measured on an unoptimised engine running a reject
> loop (see [`docs/review-findings.md`](./docs/review-findings.md) A1/A2/B9) and were
> removed rather than carried forward. Cycles→time conversions use the TSC frequency
> the engine calibrates at startup, not the spec-sheet turbo clock.
>
> **Why ingest needs concurrent clients:** `SO_REUSEPORT` distributes accepted
> *connections* across workers, so a single-socket client pins all load to one
> worker regardless of `GATEWAY_THREADS` — measured 481k orders/s at 4
> workers with one client. Load generators must open multiple connections.

## 3. Architecture
```text
Trading Firm Simulator
        │
        ▼
TCP (OUCH Protocol)
        │
        ▼
Exchange Gateway
(epoll + SO_REUSEPORT)
        │
        ▼
Lock-Free SPSC Queue
        │
        ▼
Pre-Trade Risk Engine
        │
        ▼
Matching Engine
(Price-Time Priority)
        │
        ▼
RDTSCP Telemetry Pipeline
```
The diagram above is the **ingress** path. The exchange now also closes the loop:
it writes a private `OuchExecutionReport` back on each firm's own TCP session (a
symmetric SPSC egress, `ack_queues[shard][worker]`, drained by the owning gateway
worker with non-blocking send + `EPOLLOUT` backpressure). N **genuinely-different
firms** — the same `LocalExchangeConnector` made distinct only by env (`FIRM_ID`,
`TOKEN_BASE`, `STRATEGY` = `maker` | `taker`) — apply their confirmed fills to an
ack-driven position/PnL and publish it to a per-firm `/dev/shm/firm_stats_<FIRM_ID>`
seqlock region, which the Python TUI shows as one panel per firm.

For the full multi-firm + private-ack picture, see
[docs/architecture-diagram.md](./docs/architecture-diagram.md) (the authoritative
full-system diagram, known-limitations list, and run/observe guide). For detailed
subsystem architecture, see [docs/architecture.md](./docs/architecture.md).

## 4. Technical Documentation
We treat documentation as a first-class citizen. Detailed technical deep-dives are available in the `docs/` directory:

*   [**Full-System Diagram (`docs/architecture-diagram.md`)**](./docs/architecture-diagram.md): The authoritative CLI diagram of the multi-firm + private-ack loop, the known-limitations list, and the run/observe guide. Implementation specs live in [`docs/private-ack-plan.md`](./docs/private-ack-plan.md), [`docs/multi-firm-plan.md`](./docs/multi-firm-plan.md), and [`docs/firm-monitoring-plan.md`](./docs/firm-monitoring-plan.md).
*   [**Architecture (`docs/architecture.md`)**](./docs/architecture.md): The dual-sided nature of the ecosystem, Thread-Per-Shard gateway, and Order Book design.
*   [**Benchmarks & Capacity (`docs/benchmarks.md`)**](./docs/benchmarks.md): The 5-point latency decomposition, gateway CPU cycle attribution, the measured ingest sweep, and what is still `TODO(measure)` pending reference hardware.
*   [**Benchmark Setup (`docs/benchmark-setup.md`)**](./docs/benchmark-setup.md): The three OS prerequisites (`SCHED_FIFO`, `performance` governor, `isolcpus`) that turn the lower-bound numbers into publishable ones — no code changes, environment only.
*   [**Technical Deep Dive (`docs/technical-deep-dive.md`)**](./docs/technical-deep-dive.md): Lock-Free SPSC Queues, false-sharing mitigation, Memory Pools, and atomic memory barriers.
*   [**Scheduling: SCHED_OTHER vs SCHED_FIFO (`docs/scheduling.md`)**](./docs/scheduling.md): A measured negative result — realtime scheduling is 59% *slower* on this box, why (FIFO does not timeslice equal priorities, and the engine busy-spins), and what has to be true before it wins.
*   [**Machine Profile (`docs/machine-profile.md`)**](./docs/machine-profile.md): The one laptop every number here was measured on — hybrid P/E topology, what it cannot measure, and why `config.env`'s core map is currently misaligned with the hardware.
*   [**Engineering Bottlenecks (`docs/bottlenecks.md`)**](./docs/bottlenecks.md): Challenges faced, including the TCP queueing-delay saturation, the EBADF epoll spin-loop, and the SIGBUS on the mmap'd audit log.
*   [**Telemetry Pipeline (`docs/telemetry.md`)**](./docs/telemetry.md): Using x86 hardware intrinsics to bypass `clock_gettime` overhead.

## 5. Repository Structure
```text
Trading-Ecosystem/
├── README.md               # This file
├── LICENSE                 # MIT
├── requirements.txt        # Python deps (TUI + plotting only)
├── config.env              # single config source: bash sources it, C++ getenv()s it, Python reads it
├── docs/                   # Detailed technical documentation
│   ├── architecture.md
│   ├── benchmarks.md
│   ├── benchmark-setup.md           # SCHED_FIFO / governor / isolcpus prerequisites
│   ├── bottlenecks.md
│   ├── technical-deep-dive.md
│   ├── telemetry.md
│   ├── known-issues.md               # audited bugs + resolutions
│   ├── v1.0.0-defects.md             # OPEN defects in v1.0.0 + fix plan
│   ├── review-findings.md            # senior review (A/B/C/D/E items)
│   ├── implementation-plan.md        # phased build order
│   ├── dependency.md                 # Python monitoring layout + interface contracts
│   ├── cpp-prep-for-python-monitoring.md
│   └── agent-handoff.md
├── monitoring/             # Python monitoring layer (schema/readers/TUI/orchestrator)
│   └── feeds/firm_stats_reader.py   # reads /dev/shm/firm_stats_*; discover() enumerates all firms
├── hft_engine/             # Core Exchange (Gateway, BookBuilder, Risk)
│   ├── src/
│   └── CMakeLists.txt
├── hft-trading-firm/       # Client Simulator (Load Generator, Batching)
│   ├── src/                # strategy/{istrategy,market_maker,taker}.h (maker | taker via IStrategy)
│   └── CMakeLists.txt      # CryptoPaperConnector opt-in via -DWITH_CRYPTO (default OFF)
└── scripts/                # Automation, benchmarking and analysis
    ├── run_sharding.sh           # documented benchmark entry point → results.txt
    ├── multi_firm_run.sh         # launch N firms (maker/taker), seed book, print evidence
    ├── multi_firm_evidence.py    # per-firm token-range / fill evidence for the launcher
    ├── multi_firm_demo.sh        # legacy injector-tools demo (liquidity/tester, no strategy)
    ├── decode_audit.py           # decodes order_audit.log (run_sharding.sh calls it)
    ├── measure_throughput.py     # gateway ingest sweep → benchmark_results.txt
    ├── plot.py                   # charts from results.txt
    └── setup_isolcpus.sh         # CPU isolation for measurement runs
```

## 6. Build & Run
```bash
# Clone the repository
git clone https://github.com/Kprateek283/High-Frequency-Trading-Engine.git
cd High-Frequency-Trading-Engine

# Build the Ecosystem (binaries land in build/bin/)
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
cd ..

# Run Benchmark / Stress Test (must be run from the repo root)
./scripts/run_sharding.sh

# View benchmark results
cat results.txt
```

### Multi-firm run (N competing firms + private-ack loop)
Build both sides separately; the firm needs **no** OpenSSL/curl/simdjson because the
`CryptoPaperConnector` is opt-in (`-DWITH_CRYPTO=ON`, default OFF):
```bash
cmake -S hft_engine       -B build-eng  -DCMAKE_BUILD_TYPE=Release && cmake --build build-eng  -j$(nproc)
cmake -S hft-trading-firm -B build-firm -DCMAKE_BUILD_TYPE=Release && cmake --build build-firm -j$(nproc)
```
`scripts/multi_firm_run.sh` launches `NUM_FIRMS` (default 2, max `MAX_FIRMS=16`) firms,
auto-assigning `FIRM_ID` (A,B,C,…), a disjoint `TOKEN_BASE = k*TOKEN_SLICE`
(`TOKEN_SLICE = 50M/16 = 3,125,000`), and an alternating `maker`/`taker` strategy; it
seeds the book, drives flow, and prints per-firm evidence:
```bash
BIN=build-eng FIRM_BIN=build-firm ./scripts/multi_firm_run.sh                          # 2 firms: A maker, B taker
NUM_FIRMS=4 BIN=build-eng FIRM_BIN=build-firm ./scripts/multi_firm_run.sh              # A,B,C,D (maker/taker alternating)
RUN_SECONDS=120 NUM_FIRMS=6 BIN=build-eng FIRM_BIN=build-firm ./scripts/multi_firm_run.sh   # keep alive for the TUI
```
Watch the exchange **and every firm** live (needs `rich`) — the reader's `discover()`
enumerates all `/dev/shm/firm_stats_*` regions, so every running firm gets a panel:
```bash
python3 -m monitoring.tui.app        # Exchange panel + one panel per firm (position, realized PnL, in_flight, cycles)
```
> Any per-firm fill/position figures from these runs are **illustrative lower bounds on
> a developer box**, not benchmarks — throughput/latency remain `TODO(measure)` (below).
> Strategy env knobs: `MM_SPREAD` / `MM_IMBALANCE` / `MM_SIZE` (maker),
> `TAKER_THRESHOLD` / `TAKER_SIZE` (taker).

### Python monitoring layer (optional)
The C++ engine above needs nothing from Python. The `monitoring/` package is
stdlib-only through tier 4; only the live TUI (`rich`) and the plotting script
(`matplotlib`) have external dependencies, so the test suite runs with nothing
installed at all — it skips the modules whose dependencies are missing.

```bash
python3 -m monitoring.run_tests              # works as-is; skips the TUI without rich

# For the TUI and plots, use a virtual environment. Debian/Ubuntu refuse
# system-wide pip installs (PEP 668), so this is the portable route:
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt    # rich + matplotlib

.venv/bin/python -m monitoring.run_tests     # full suite, nothing skipped
.venv/bin/python -m monitoring.tui.app       # live dashboard against a running engine
```

### Sample Benchmark Output
```text
Gateway Threads : 4
entries       : 30000
NEW           : 10000
matches       : 20000
FILLED        : 20000
```
> Throughput/latency numbers are pending re-measurement on reference hardware
> (`TODO(measure)`); the run above verifies the pipeline end-to-end and that
> matches are produced.

### Measuring latency & the capacity matrix
The harness is complete and needs no code changes — only a box configured for
deterministic execution. Set the three OS prerequisites once
([`docs/benchmark-setup.md`](./docs/benchmark-setup.md)):

1. **`SCHED_FIFO`** — install `scripts/99-hft-realtime.conf` into `/etc/security/limits.d/` and re-login (`ulimit -r` cannot raise its own hard limit). Else real-time pinning silently degrades; the exchange prints `RT_SCHED: granted=N/M` so you can tell.
2. **`performance` governor** — `sudo cpupower frequency-set -g performance`.
3. **`isolcpus`** — isolate the engine's cores (1–8, per `config.env`) via GRUB + reboot.

Then produce the full matrix with two commands:
```bash
# Latency percentiles + cycle attribution + accepted/rejected, per shard count
for gt in 1 2 4 8; do GATEWAY_THREADS=$gt ./scripts/run_sharding.sh; done   # -> results.txt

# Ingest throughput sweep (gateway workers x concurrent clients)
python3 scripts/measure_throughput.py                                       # -> benchmark_results.txt
```
Both files stamp the environment (`governor`, `rtprio_limit`, `isolcpus`) in their
header, so a lower-bound run is always distinguishable from a publishable one. On this
development laptop the numbers are a lower bound; see `docs/benchmarks.md`.

## 7. Future Work
1. **Kernel Bypass:** Implement Intel DPDK or Solarflare `ef_vi` to map the NIC directly to userspace memory.
2. **Benchmark Re-measurement:** Run the full throughput/latency capacity matrix on an idle reference box and populate the `TODO(measure)` figures (Phase 3.5).
3. **Protocol Optimization:** Transition from TCP to a custom Reliable UDP for order entry.

> Thread affinity (`pthread_setaffinity_np` + `SCHED_FIFO`) is **already implemented** for
> the engine, publisher, gateway, and — since Phase 3.2 — the gateway worker threads; it is
> no longer future work. Market data already disseminates over ITCH multicast.
