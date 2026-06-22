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

### Out-of-Order Execution & Memory Fences
Modern CPUs are highly superscalar and will reorder instructions to maximize throughput. If we simply call `__rdtsc`, the CPU might execute the timestamp read *before* the code we are trying to measure.

To guarantee accurate timing boundaries, we pair the timestamp read with a Load Fence (`_mm_lfence()`), which forces the CPU to complete all prior memory loads before executing the timestamp capture:

```cpp
inline uint64_t get_cycles() {
    uint32_t aux;
    _mm_lfence();
    return __rdtscp(&aux);
}
```

The overhead of the measurement itself is small relative to the events being measured, making it suitable for cycle-level attribution.

---

## The Timestamp Pipeline

To track an order through the entire ecosystem, the system captures cycle counts at three distinct lifecycle boundaries:

1. **`T0` (Client Origin):** The Trading Firm Simulator injects `T0` directly into the binary payload of the simulated OUCH network packet before calling `send()`.
2. **`T1` (Gateway Ingress):** The Exchange Gateway captures `T1` the exact moment the `Order` object is decoded from the `read()` buffer.
3. **`T2` (Engine Egress):** The Matching Engine captures `T2` the moment the `Order` finishes processing (e.g., resting in the book or triggering a trade).

This three-point decomposition provides absolute visibility into where CPU cycles are being spent.

### Latency Decomposition

By subtracting these timestamps, we can isolate the performance of distinct subsystems:

*   **TCP / Queueing Delay:** `T1 - T0` 
    *(Measures the cost of `send()`, TCP loopback, socket buffering, and `epoll_wait` / `read()`)*
*   **Engine Execution Time:** `T2 - T1`
    *(Measures the cost of the SPSC queue handoff, pre-trade risk validation, and limit order book matching)*
*   **End-to-End Latency:** `T2 - T0`

## Example Timeline

```text
T0 ---------------------- T1 ----------- T2
|                         |              |
Client Send          Gateway Decode   Match Complete

T1 - T0 = TCP Path + Queueing Delay
T2 - T1 = Engine Execution
T2 - T0 = End-to-End Latency
```

---

## Off-Path Logging

Capturing cycle counts is fast, but logging them to `stdout` or writing them to disk requires highly expensive OS locks and I/O wait times.

To ensure the telemetry pipeline never slows down the trading path, the system employs an asynchronous, off-path logging strategy. The core Matching Engine thread writes its `(T0, T1, T2)` tuples into a dedicated lock-free SPSC queue. A dedicated background telemetry thread reads from this queue and aggregates statistics asynchronously.

This guarantees that the application logic runs unencumbered. This telemetry infrastructure made it possible to attribute latency to specific subsystems and ultimately identify the Linux networking path as the dominant source of ingress overhead under sustained load.
