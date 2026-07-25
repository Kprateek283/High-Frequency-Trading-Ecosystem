# Ecosystem Architecture

## 1. End-to-End Data Flow

```text
┌─────────────────────────────────────────────────────────────────────────────┐
│                       Trading Firm Simulator (Client)                       │
│                                                                             │
│  [Load Generator Thread]                                                    │
│  - Generates binary OUCH protocol payloads                                  │
│  - Application-Level Batching (Groups orders per send() syscall)            │
│  - Injects `__rdtscp` timestamps (t1..t4) into packet payload               │
└───────┬─────────────────────────────────────────────────────────────────────┘
        │
        ▼  TCP/IP over Loopback (batching amortises the send() syscall)
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
│  - epoll_wait(EPOLLET) -> Edge-Triggered Event Loop (client fds)            │
│  - read(O_NONBLOCK) -> Drains kernel buffer to userspace                    │
│  - memcpy into aligned OuchEnterOrder (cast would be UB; memcpy elides)     │
│  - Captures Gateway ingress Timestamp (t5)                                  │
└───────┬─────────────────────────────────────────────────────────────────────┘
        │
        ▼  Userspace Memory Boundary
        │
┌───────┴─────────────────────────────────────────────────────────────────────┐
│                       Lock-Free Inter-Thread Boundary                       │
│                                                                             │
│  [SPSC Ring Buffer]  (per-[shard][gateway-worker], fan-in)                  │
│  - atomic head alignas(128); <-- distinct cache-line PAIR (L2 prefetcher)   │
│  - atomic tail alignas(128); <-- Prevents hardware false-sharing            │
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
│  - Pairs task ingress_tsc with match-time get_tsc() (engine execution)      │
└───────┬─────────────────────────────────────────────────────────────────────┘
        │
        ▼
┌───────┴─────────────────────────────────────────────────────────────────────┐
│                         RDTSCP Telemetry Pipeline                           │
│                                                                             │
│  - Dumps (ingress_tsc, match_tsc) to a lock-free telemetry logger           │
│  - __rdtscp retires prior loads before reading TSC (no separate lfence)     │
└─────────────────────────────────────────────────────────────────────────────┘
```

The diagram above is the **ingress** path. Every match also fans out on **three
channels** — public market data, a private ack, and the drop copy:

```text
   Matching Engine (per fill, BOTH sides of the trade)
        |
        +--> mkt_data_q --> Publisher --> ITCH multicast (UDP, anonymous)     [public]
        |
        +--> ack_queues[shard][worker] (OuchExecutionReport, SPSC egress)     [private]
        |       routed by order->worker_id --> owning Gateway worker drains
        |       after epoll_wait --> writes back on the originating client fd
        |       (non-blocking send + EPOLLOUT backpressure)
        |
        +--> drop_copy_q --> OrderManager --> order_audit.log + /dev/shm/hft_stats  [audit]
```

The private-ack egress **mirrors the ingress SPSC fabric exactly**: engine shard `s`
is the sole producer into `ack_queues[s][*]`, gateway worker `w` the sole consumer of
`ack_queues[*][w]` (see Component Breakdown item 5). The full multi-firm + ack-loop
picture is in [architecture-diagram.md](./architecture-diagram.md).

## 2. Threading Model

Under `SO_REUSEPORT`, any gateway worker can receive an order for **any** instrument, so
the ingress fabric is not 1:1. Each gateway worker `w` owns a **private** SPSC queue into
**every** engine shard `s` — `queues[s][w]` — and each engine drains its whole row across
all workers. With `GATEWAY_THREADS=4` and `NUM_SHARDS=4` that is **16** ingress queues, not
4. Every queue still has exactly one producer and one consumer (the SPSC invariant that a
naive 1:1 diagram would violate the moment two workers routed to the same shard):

```text
   ┌──────────┐   ┌──────────┐   ┌──────────┐   ┌──────────┐
   │ Gateway 1│   │ Gateway 2│   │ Gateway 3│   │ Gateway 4│   (each: epoll loop,
   └────┬─────┘   └────┬─────┘   └────┬─────┘   └────┬─────┘    inst % NUM_SHARDS)
        │  ┌───────────┼───────────────┼────────────┐│
        │  │           │  ┌────────────┼──────┐     ││   release → acquire
        ▼  ▼           ▼  ▼            ▼  ▼    ▼     ▼▼   per-[shard][worker] SPSC
   ┌──────────┐   ┌──────────┐   ┌──────────┐   ┌──────────┐
   │ q[0][*]  │   │ q[1][*]  │   │ q[2][*]  │   │ q[3][*]  │   (4 queues each = 16)
   └────┬─────┘   └────┬─────┘   └────┬─────┘   └────┬─────┘
        ▼              ▼              ▼              ▼
   ┌──────────┐   ┌──────────┐   ┌──────────┐   ┌──────────┐
   │ Engine 0 │   │ Engine 1 │   │ Engine 2 │   │ Engine 3 │   (drains its row of
   │Risk+Match│   │Risk+Match│   │Risk+Match│   │Risk+Match│    4 worker queues)
   └──────────┘   └──────────┘   └──────────┘   └──────────┘
```

(Gateway rejects fan in the same way to the `OrderManager` via per-worker
`gw_reject_queues`; the engine's own drop-copy queue is engine-only.)

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
The system utilizes custom Single-Producer Single-Consumer (SPSC) ring buffers. Memory visibility is guaranteed strictly through C++11 `<atomic>` fences (`acquire` / `release`). As shown in the Threading Model diagram, the ingress fabric is a per-`[shard][worker]` **fan-in**: each gateway worker owns a private queue into every engine shard, and each engine drains its whole row — preserving the single-producer/single-consumer invariant even though any worker can route to any shard. This completely decoupled pipeline allows 100% concurrent execution from network ingestion to order matching.

**False Sharing Mitigation:** A critical hardware-level optimization. If the Gateway thread updates the queue's `tail` while the Matching thread updates `head` and both share a cache line, the silicon constantly invalidates the CPU cache (False Sharing). The queue uses `alignas(128)` — not 64 — because Intel's L2 spatial prefetcher pulls in 128-byte-aligned cache-line *pairs*, so 64-byte separation still false-shares on this CPU family; 128-byte alignment forces the members onto separate line pairs.

### 3. Deterministic Memory Management
Standard `new` and `malloc` invoke the OS memory manager, which requires locks and has non-deterministic execution times. The entire hot path of this ecosystem features **Zero Dynamic Allocations**.
At startup, massive contiguous byte arrays are pre-allocated. When an order arrives, `placement new` is used to construct the C++ object directly into the existing memory block. This ensures that the engine never suffers from allocation jitter or page faults during live trading.

### 4. Telemetry Pipeline
Measuring sub-microsecond events using conventional software timers introduces significant observer overhead. Even with vDSO optimizations, `clock_gettime()` may still introduce measurable observer overhead relative to direct TSC reads.
The system bypasses software timers entirely. We use the `__rdtscp` compiler intrinsic to read the CPU's internal cycle counter directly from the silicon. `__rdtscp` (unlike `__rdtsc`) waits for prior instructions to retire before reading the counter, so it already serialises against prior loads — no separate `_mm_lfence()` is required.

### 5. Private-Ack Egress (symmetric SPSC)
The exchange writes a private `OuchExecutionReport` back to the originating client on
its own order-entry socket. Because a client fd is owned by exactly one gateway worker
but fills are produced on engine threads, each report is routed back to the owning
worker over an **egress fabric that mirrors ingress**: `ack_queues[shard][worker]`, where
engine shard `s` is the sole producer into `ack_queues[s][*]` and gateway worker `w` the
sole consumer of `ack_queues[*][w]` — the same single-producer/single-consumer invariant
as the ingress `queues[shard][worker]`. `Order` carries a `worker_id` (stamped at
allocation) so both sides of every fill route to the right worker. The worker drains its
column after `epoll_wait` and writes non-blocking (`send` + `MSG_NOSIGNAL`), arming
`EPOLLOUT` on a partial send and closing a client whose out-buffer overflows. This leaves
`DropCopyMessage`, the audit format, and the entire Python layer untouched (see
[`private-ack-plan.md`](./private-ack-plan.md)).

### 6. Multi-Firm & Firm Monitoring
N trading firms run as separate processes of the **same** `LocalExchangeConnector`, made
distinct only by env: `FIRM_ID` (stamped in `req.firm`), `TOKEN_BASE` (partitions the
exchange's 50M `SessionManager` token space into `MAX_FIRMS=16` slices of
`TOKEN_SLICE=3,125,000` so firms never collide), and `STRATEGY` = `maker` | `taker`
(via the `IStrategy` interface). Each firm's `tcp_rx_thread` applies its confirmed
`OuchExecutionReport`s to an **ack-driven** position + realized PnL, then publishes them
(every ~100 ms, single writer) to a per-firm seqlock region at
`/dev/shm/firm_stats_<FIRM_ID>` — the same pattern as the exchange's `/dev/shm/hft_stats`.
The Python `firm_stats_reader.discover()` enumerates every firm region, so the TUI shows
one panel per participant alongside the exchange (see
[`multi-firm-plan.md`](./multi-firm-plan.md), [`firm-monitoring-plan.md`](./firm-monitoring-plan.md)).
