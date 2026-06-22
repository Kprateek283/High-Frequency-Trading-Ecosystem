# High-Frequency Trading Ecosystem

## 1. Project Overview
A cycle-accurate High-Frequency Trading (HFT) ecosystem built in modern C++ to explore the performance limits of POSIX userspace networking without kernel-bypass technologies such as DPDK.

The project models both sides of a trading venue: a Trading Firm Simulator capable of generating 10M+ messages/sec and an Exchange comprising a sharded TCP Gateway, lock-free ingestion pipeline, Pre-Trade Risk Engine, and deterministic Price-Time Priority Matching Engine.

## 2. Key Results
| Metric | Result |
| :--- | :--- |
| **Sustained Throughput** | 10,000,000+ messages/sec |
| **End-to-End Latency** | 1.42M CPU cycles (~355 µs) |
| **Core Business Logic** | 350–370 CPU cycles (<100 ns) |
| **Kernel Networking Overhead** | ~77% of ingestion latency |
| **Gateway Architecture** | 4-thread `SO_REUSEPORT` sharding |

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
*   [**Engineering Bottlenecks (`docs/bottlenecks.md`)**](./docs/bottlenecks.md): Challenges faced, including overcoming the 75ms TCP queueing delay and the epoll spin-loop bug.
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
│   └── telemetry.md
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

# Build the Ecosystem
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# Run Benchmark / Stress Test
./scripts/run_sharding.sh

# View detailed benchmark results
cat new_results.txt
```

### Sample Benchmark Output
```text
Throughput      : 10,000,000 msgs/sec
End-to-End      : 1,422,379 cycles
Trading Engine  : 350 cycles
Gateway Threads : 4
```

## 7. Future Work
1. **Kernel Bypass:** Implement Intel DPDK or Solarflare `ef_vi` to map the NIC directly to userspace memory.
2. **NUMA Thread Affinity:** Use `pthread_setaffinity_np` to explicitly pin Gateway and Matching threads to specific physical CPU cores.
3. **Protocol Optimization:** Transition from TCP to a custom Reliable UDP or Multicast architecture for market data dissemination.
