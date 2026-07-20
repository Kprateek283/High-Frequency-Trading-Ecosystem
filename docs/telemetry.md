# Cycle-Accurate Telemetry

A critical question in any low-latency systems benchmark is: *"How do you know the bottleneck was in the kernel networking stack and not your application code?"*

To definitively answer this without relying on speculative profiling, the ecosystem implements a low-overhead, cycle-accurate telemetry pipeline based directly on hardware CPU instructions.

---

## The Problem with Standard Timers

Standard C++ timing APIs such as `std::chrono::high_resolution_clock` are typically implemented via `clock_gettime()`, often accelerated through Linux's vDSO mechanism. 

While vDSO optimizations avoid a full context switch, invoking the time API still incurs a measurable execution cost (often 20–50 nanoseconds depending on the hardware). When your entire business logic executes in roughly 100 nanoseconds, injecting multiple `clock_gettime()` calls into the hot path fundamentally alters the system's performance. The act of measuring the latency becomes a major source of the latency (the Heisenberg effect).

---

## Hardware Cycle Counting (`__rdtscp`)

To achieve near-zero-overhead observability, the system bypasses software abstractions entirely and interacts directly with the silicon.

Modern x86 processors expose a Time Stamp Counter (TSC), a monotonically increasing hardware counter that can be read directly from userspace using the `__rdtsc` or `__rdtscp` compiler intrinsics.

### Out-of-Order Execution & Serialisation
Modern CPUs are highly superscalar and will reorder instructions to maximize throughput. A plain `__rdtsc` can be executed *before* the loads we are trying to measure have retired.

The system uses `__rdtscp` rather than `__rdtsc` precisely to avoid this: `__rdtscp` waits for all prior instructions to retire before reading the counter, so it already serialises against prior loads — no separate `_mm_lfence()` is needed. The real function (`core/timer.h`):

```cpp
inline uint64_t get_tsc() {
    unsigned int dummy;
    return __rdtscp(&dummy);
}
```

The overhead of the measurement itself is small relative to the events being measured, making it suitable for cycle-level attribution.

---

## The Timestamp Pipeline

To track an order through the entire ecosystem, the system captures cycle counts at **five** lifecycle boundaries. Four of them ride on the wire inside `OuchEnterOrder` (`protocol/messages.h`); the gateway captures the fifth on ingress:

1. **`t1_exchange_send`:** stamped on the firm side as the order's send origin, carried in the OUCH payload.
2. **`t2_trading_recv`:** the firm receives the market-data tick that triggers the order.
3. **`t3_trading_enq`:** the firm enqueues the outbound order action.
4. **`t4_network_deq`:** the firm dequeues the action just before `send()`.
5. **`t5` (Gateway Ingress):** the Exchange Gateway stamps the moment `read()` returns the bytes (`gateway/tcp_server.h`).

Separately, the Matching Engine attributes its own execution by pairing the task's `ingress_tsc` with a `get_tsc()` at match completion (`matching/engine.cpp`).

This decomposition provides visibility into where CPU cycles are being spent.

### Latency Decomposition

By subtracting these timestamps, we isolate the performance of distinct subsystems:

*   **Firm-internal path:** `t4_network_deq - t2_trading_recv` *(tick receipt → enqueue → dequeue)*
*   **TCP / Queueing Delay:** `t5 - t4_network_deq` *(`send()`, TCP loopback, socket buffering, `epoll_wait` / `read()`)*
*   **End-to-End Latency:** `t5 - t1_exchange_send`
*   **Engine Execution Time:** the engine's `ingress_tsc → match` pair (SPSC handoff + risk + matching)

## Example Timeline

```text
t1 -------- t2 ---- t3 ---- t4 -------------- t5
|           |       |       |                 |
Firm send   Firm    Firm    Firm dequeue   Gateway read()
origin      recv    enqueue (pre-send)     returns

t5 - t4 = TCP Path + Queueing Delay
t5 - t1 = End-to-End Latency
```

---

## Off-Path Logging

Capturing cycle counts is fast, but logging them to `stdout` or writing them to disk requires highly expensive OS locks and I/O wait times.

To ensure the telemetry pipeline never slows down the trading path, the system employs an asynchronous, off-path logging strategy. The core Matching Engine thread writes its `(ingress_tsc, match_tsc)` tuples into a dedicated lock-free SPSC queue. A dedicated background telemetry thread reads from this queue and aggregates statistics asynchronously.

This guarantees that the application logic runs unencumbered. This telemetry infrastructure made it possible to attribute latency to specific subsystems and ultimately identify the Linux networking path as the dominant source of ingress overhead under sustained load.
