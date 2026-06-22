# Ecosystem Architecture

## 1. End-to-End Data Flow

```text
┌─────────────────────────────────────────────────────────────────────────────┐
│                       Trading Firm Simulator (Client)                       │
│                                                                             │
│  [Load Generator Thread]                                                    │
│  - Generates binary OUCH protocol payloads                                  │
│  - Application-Level Batching (Groups 100+ orders per send() syscall)       │
│  - Injects `__rdtscp` timestamp (T0) into packet payload                    │
└───────┬─────────────────────────────────────────────────────────────────────┘
        │
        ▼  TCP/IP over Loopback (Batching mitigates syscall overhead by 170x)
        │
┌───────┴─────────────────────────────────────────────────────────────────────┐
│                           Linux Kernel Space                                │
│                                                                             │
│  [TCP Receive Buffers]  <-- Major source of queueing delay under saturation │
│  [SO_REUSEPORT Hash]    <-- Load balances connections to multiple threads   │
└───────┬─────────────────────────────────────────────────────────────────────┘
        │
        ▼  Context Switch Boundary
        │
┌───────┴─────────────────────────────────────────────────────────────────────┐
│                         Exchange Gateway (Ingress)                          │
│                                                                             │
│  [Thread-Per-Shard Model (e.g., 4 Threads)]                                 │
│  - epoll_wait(EPOLLET) -> Edge-Triggered Event Loop                         │
│  - read(O_NONBLOCK) -> Drains kernel buffer to userspace                    │
│  - Zero-Copy Cast -> `reinterpret_cast<const Order*>(char_buffer)`          │
│  - Captures Gateway Timestamp (T1)                                          │
└───────┬─────────────────────────────────────────────────────────────────────┘
        │
        ▼  Userspace Memory Boundary
        │
┌───────┴─────────────────────────────────────────────────────────────────────┐
│                       Lock-Free Inter-Thread Boundary                       │
│                                                                             │
│  [SPSC Ring Buffer]                                                         │
│  - size_t head alignas(64); <-- Forces head pointer to distinct cache line  │
│  - size_t tail alignas(64); <-- Prevents hardware false-sharing             │
│  - Gateway Producer : std::memory_order_release                             │
│  - Engine Consumer  : std::memory_order_acquire                             │
│                                                                             │
│  [Deterministic Memory Pool]                                                │
│  - placement new(pool_ptr) Order; <-- Zero dynamic heap allocations         │
└───────┬─────────────────────────────────────────────────────────────────────┘
        │
        ▼  Lock-Free Userspace Handoff
        │
┌───────┴─────────────────────────────────────────────────────────────────────┐
│                     Matching Engine (Business Logic)                        │
│                                                                             │
│  [Pre-Trade Risk Engine]                                                    │
│  - Synchronous fat-finger and margin validation                             │
│                                                                             │
│  [BookBuilder / Order Book]                                                 │
│  - Deterministic Price-Time Priority Matching                               │
│  - L2 Depth modeled via flat arrays + doubly linked lists (O(1) updates)    │
│  - Captures Execution Timestamp (T2)                                        │
└───────┬─────────────────────────────────────────────────────────────────────┘
        │
        ▼
┌───────┴─────────────────────────────────────────────────────────────────────┐
│                         RDTSCP Telemetry Pipeline                           │
│                                                                             │
│  - Dumps (T0, T1, T2) directly to a lock-free telemetry logger              │
│  - _mm_lfence() prevents CPU speculative execution from corrupting cycles   │
└─────────────────────────────────────────────────────────────────────────────┘
```

## 2. Threading Model

```text
       ┌───────────────┐ ┌───────────────┐ ┌───────────────┐ ┌───────────────┐
       │   Gateway 1   │ │   Gateway 2   │ │   Gateway 3   │ │   Gateway 4   │
       │  (epoll loop) │ │  (epoll loop) │ │  (epoll loop) │ │  (epoll loop) │
       └───────┬───────┘ └───────┬───────┘ └───────┬───────┘ └───────┬───────┘
               │                 │                 │                 │
         memory_order_     memory_order_     memory_order_     memory_order_
           release           release           release           release
               │                 │                 │                 │
               ▼                 ▼                 ▼                 ▼
       ┌───────────────┐ ┌───────────────┐ ┌───────────────┐ ┌───────────────┐
       │ SPSC Queue 1  │ │ SPSC Queue 2  │ │ SPSC Queue 3  │ │ SPSC Queue 4  │
       └───────┬───────┘ └───────┬───────┘ └───────┬───────┘ └───────┬───────┘
               │                 │                 │                 │
         memory_order_     memory_order_     memory_order_     memory_order_
           acquire           acquire           acquire           acquire
               │                 │                 │                 │
               │                 │                 │                 │
               ▼                 ▼                 ▼                 ▼
       ┌───────────────┐ ┌───────────────┐ ┌───────────────┐ ┌───────────────┐
       │   Engine 1    │ │   Engine 2    │ │   Engine 3    │ │   Engine 4    │
       │ (Risk+Match)  │ │ (Risk+Match)  │ │ (Risk+Match)  │ │ (Risk+Match)  │
       └───────────────┘ └───────────────┘ └───────────────┘ └───────────────┘
```

## Design Goals
- Deterministic latency
- Zero dynamic allocations in the hot path
- Lock-free inter-thread communication
- Cycle-accurate telemetry
- Maximum throughput using standard POSIX networking

## Component Breakdown

### 1. The Linux Kernel Boundary (`SO_REUSEPORT`)
The most significant architectural decision is the reliance on the Linux kernel to natively load-balance TCP connections. By binding multiple Gateway threads to the same port using `SO_REUSEPORT`, the kernel hashes the incoming source IP/Port and pins each TCP stream to a specific `epoll` thread. This avoids the need for a dedicated acceptor-to-worker handoff design and allows the Linux kernel to distribute connections directly across gateway threads.

### 2. Lock-Free SPSC Queues
Standard queues use `std::mutex` and `std::condition_variable`, which force the operating system to put waiting threads to sleep (`futex` syscalls). At the measured latencies of this system, a context switch can exceed the cost of processing an order through the core business logic.
The system utilizes custom Single-Producer Single-Consumer (SPSC) ring buffers. Memory visibility is guaranteed strictly through C++11 `<atomic>` fences (`acquire` / `release`). As shown in the Threading Model diagram, each Gateway thread pushes to its own dedicated SPSC queue, which is then paired 1-to-1 with a dedicated Matching Engine thread. This completely decoupled pipeline allows 100% concurrent execution from network ingestion to order matching.

**False Sharing Mitigation:** A critical hardware-level optimization. CPU L1/L2 caches operate in 64-byte blocks (cache lines). If the Gateway thread updates the queue's `head` pointer while the Matching thread updates the `tail` pointer, and both pointers sit within the same 64 bytes of memory, the silicon hardware will constantly invalidate the CPU cache (False Sharing). By explicitly using `alignas(64)`, the pointers are forced apart in physical memory, neutralizing CPU thrashing.

### 3. Deterministic Memory Management
Standard `new` and `malloc` invoke the OS memory manager, which requires locks and has non-deterministic execution times. The entire hot path of this ecosystem features **Zero Dynamic Allocations**.
At startup, massive contiguous byte arrays are pre-allocated. When an order arrives, `placement new` is used to construct the C++ object directly into the existing memory block. This ensures that the engine never suffers from allocation jitter or page faults during live trading.

### 4. Telemetry Pipeline
Measuring sub-microsecond events using conventional software timers introduces significant observer overhead. Even with vDSO optimizations, `clock_gettime()` may still introduce measurable observer overhead relative to direct TSC reads.
The system bypasses software timers entirely. We use the `__rdtscp` compiler intrinsic to read the CPU's internal cycle counter directly from the silicon. We pair this with `_mm_lfence()` (Load Fence) to stop the CPU from executing instructions out-of-order, guaranteeing cycle-accurate telemetry for latency decomposition.
