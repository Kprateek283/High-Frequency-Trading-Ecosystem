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
| **Sustained Throughput** | `TODO(measure)` — re-run on reference hardware |
| **End-to-End Latency** | `TODO(measure)` |
| **Gateway Ingest Path** | `TODO(measure)` — Decode + Validate + Enqueue (this is the ingest path, *not* the matching engine, which is measured separately) |

> Throughput, latency, and cycle figures are pending re-measurement on an idle reference
> box (Phase 3.5 stop condition). Earlier headline numbers were measured on an unoptimised
> engine running a reject loop (see [`docs/review-findings.md`](./docs/review-findings.md)
> A1/A2/B9) and were removed rather than carried forward. Cycles→time conversions use the
> TSC frequency the engine calibrates at startup, not the spec-sheet turbo clock.

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
├── docs/                   # Detailed technical documentation
│   ├── architecture.md
│   ├── benchmarks.md
│   ├── bottlenecks.md
│   ├── technical-deep-dive.md
│   ├── telemetry.md
│   ├── known-issues.md               # audited bugs + resolutions
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
└── scripts/                # Bash automation and build scripts
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
(`matplotlib`) have external dependencies.
```bash
python3 -m pip install -r requirements.txt   # rich + matplotlib

python3 -m monitoring.run_tests              # module self-tests (skips what isn't installed)
python3 -m monitoring.tui.app                # live dashboard against a running engine
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
