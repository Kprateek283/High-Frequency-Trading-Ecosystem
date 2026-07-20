# High-Frequency Trading Ecosystem

## 1. Project Overview
A cycle-accurate High-Frequency Trading (HFT) ecosystem built in modern C++ to explore the performance limits of POSIX userspace networking without kernel-bypass technologies such as DPDK.

The project models both sides of a trading venue: a Trading Firm Simulator capable of generating 10M+ messages/sec and an Exchange comprising a sharded TCP Gateway, lock-free ingestion pipeline, Pre-Trade Risk Engine, and deterministic Price-Time Priority Matching Engine.

## 2. Key Results
| Metric | Result |
| :--- | :--- |
| **Functional run** | 4-thread gateway matches orders end-to-end: 10,000 orders → 20,000 fills, **0 rejects** (`results.txt`) |
| **Gateway Architecture** | 4-thread `SO_REUSEPORT` sharding |
| **Latency probes** | 5-point `__rdtscp` decomposition (4 on-wire + gateway ingress) |
| **Gateway ingest** | **~1.08M orders/sec** at 4 workers × 4 concurrent clients; scales 215k → 587k → 1.08M as clients are added (`benchmark_results.txt`) |
| **Gateway Ingest Path** | Decode **~82**, Validate **~24**, Enqueue **~340** cycles/order (ingest path only, *not* the matching engine) |
| **End-to-End Latency** | `TODO(measure)` — needs a box where `SCHED_FIFO` is grantable; see below |

> **Read the throughput number with its caveat.** It was measured by
> [`scripts/measure_throughput.py`](./scripts/measure_throughput.py), which samples
> `orders_in` from the stats region rather than trusting a client's send rate (a
> `send()` returns once buffered, so client-side "throughput" is offered load, not
> work done). It is a **lower bound from a developer desktop**: no `SCHED_FIFO`
> privilege, a `powersave` governor, and other applications running. The *shape* —
> that ingest scales with concurrent connections and saturates near 4 — is the
> meaningful result; the ceiling will be higher on isolated hardware. Re-run the
> script there to replace it.
>
> **Latency percentiles are deliberately still unmeasured.** Tail latency is
> precisely what arbitrary preemption destroys, and this box cannot grant
> `SCHED_FIFO` (`ulimit -r` is 0), so p99/p99.9 figures from it would be
> measuring the scheduler, not the engine. Publishing them would repeat the
> mistake this project already corrected once.
>
> Earlier headline numbers were measured on an unoptimised engine running a reject
> loop (see [`docs/review-findings.md`](./docs/review-findings.md) A1/A2/B9) and were
> removed rather than carried forward. Cycles→time conversions use the TSC frequency
> the engine calibrates at startup, not the spec-sheet turbo clock.
>
> **Why ingest needs concurrent clients:** `SO_REUSEPORT` distributes accepted
> *connections* across workers, so a single-socket client pins all load to one
> worker regardless of `GATEWAY_THREADS` — measured 215k orders/s at both 1 and 4
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
For detailed architecture, see: [docs/architecture.md](./docs/architecture.md)

## 4. Technical Documentation
We treat documentation as a first-class citizen. Detailed technical deep-dives are available in the `docs/` directory:

*   [**Architecture (`docs/architecture.md`)**](./docs/architecture.md): The dual-sided nature of the ecosystem, Thread-Per-Shard gateway, and Order Book design.
*   [**Benchmarks & Capacity (`docs/benchmarks.md`)**](./docs/benchmarks.md): Telemetry proving the 10M msgs/sec scaling and Gateway CPU cycle attribution.
*   [**Technical Deep Dive (`docs/technical-deep-dive.md`)**](./docs/technical-deep-dive.md): Lock-Free SPSC Queues, false-sharing mitigation, Memory Pools, and atomic memory barriers.
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
├── hft_engine/             # Core Exchange (Gateway, BookBuilder, Risk)
│   ├── src/
│   └── CMakeLists.txt
├── hft-trading-firm/       # Client Simulator (Load Generator, Batching)
│   ├── src/
│   └── CMakeLists.txt
└── scripts/                # Automation, benchmarking and analysis
    ├── run_sharding.sh           # documented benchmark entry point → results.txt
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

## 7. Future Work
1. **Kernel Bypass:** Implement Intel DPDK or Solarflare `ef_vi` to map the NIC directly to userspace memory.
2. **Benchmark Re-measurement:** Run the full throughput/latency capacity matrix on an idle reference box and populate the `TODO(measure)` figures (Phase 3.5).
3. **Protocol Optimization:** Transition from TCP to a custom Reliable UDP for order entry.

> Thread affinity (`pthread_setaffinity_np` + `SCHED_FIFO`) is **already implemented** for
> the engine, publisher, gateway, and — since Phase 3.2 — the gateway worker threads; it is
> no longer future work. Market data already disseminates over ITCH multicast.
