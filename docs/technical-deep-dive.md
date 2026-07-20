# Technical Deep Dive

Achieving 10,000,000 messages per second requires abandoning standard application-level abstractions in favor of mechanical sympathy. This document details the specific C++ techniques used to optimize the hot path, ensuring deterministic sub-microsecond execution times.

---

## 1. Zero Dynamic Allocations (`placement new`)

### The Problem
Standard heap allocations (`new` or `malloc`) rely on the general-purpose userspace allocator. Under high contention, allocator metadata management and synchronization can become a bottleneck. Furthermore, finding contiguous free memory is non-deterministic, meaning an allocation could take 50ns or trigger a 5,000ns page fault.

### The Implementation
The system pre-allocates massive, contiguous byte arrays at startup. During live trading, the hot path strictly uses **Zero Dynamic Allocations**.

When a new order arrives, the Gateway calls the pool's variadic `allocate()`, which performs the `placement new` *itself* and returns an already-constructed `Order*` (`core/memory_pool.h`). `Order`'s constructor takes six fields (`matching/order.h`), not three:
```cpp
Order* o = pool.allocate(internal_id, order_token, client_id, price, shares, inst, side);
```
The allocate fast path is guarded by a spinlock (`alloc_lock`), not lock-free: multiple gateway workers can allocate from the same shard pool concurrently, so the high-water bump must be serialised. The engine's `deallocate()` stays lock-free.

**Impact:** Order creation avoids general-purpose heap allocation and reduces allocation overhead to a bounded constant-time operation.

---

## 2. Lock-Free Queues & Memory Visibility

### The Problem
The Gateway threads must hand off orders to the Matching Engine threads. Standard `std::queue` with `std::mutex` forces threads to sleep when contended, introducing context-switch penalties that exceed the cost of the entire business logic.

### The Implementation
The system uses custom **Single-Producer Single-Consumer (SPSC)** ring buffers. Because only one thread pushes and one thread pops, we can completely eliminate locks.

Synchronization is handled purely through hardware memory barriers using C++11 `<atomic>`:
```cpp
// Producer (Gateway)
ring_buffer[tail_local] = order;
tail.store(tail_local + 1, std::memory_order_release);

// Consumer (Matching Engine)
if (head.load(std::memory_order_acquire) < tail_local) { ... }
```
*   `memory_order_release`: Guarantees that prior writes become visible before publication of the updated tail pointer.
*   `memory_order_acquire`: Guarantees that subsequent reads observe data published before the corresponding release operation.

**Impact:** Threads never sleep or incur kernel-mediated synchronization overhead. Communication is reduced to atomic loads and stores on shared memory.

---

## 3. False-Sharing Mitigation (`alignas(128)`)

### The Problem
Modern CPU caches operate in 64-byte chunks called "Cache Lines". In the SPSC queue, the Gateway thread constantly writes to `tail`, and the Matching Engine constantly writes to `head`. 
If `head` and `tail` share a cache line, every `tail` update invalidates the consumer's line containing `head`, forcing a slow fetch from main memory. This hardware-level thrashing is known as **False Sharing**.

### The Implementation
We separate the members with `alignas(128)`, not 64 (`core/lock_free_queue.h`):
```cpp
alignas(128) std::atomic<size_t> head;
alignas(128) std::atomic<size_t> tail;
```
128, not 64, is deliberate: Intel's L2 **spatial prefetcher** pulls in 128-byte-aligned pairs of cache lines, so 64-byte separation still lets the two lines be prefetched together and false-share on this CPU family. 128-byte alignment defeats that pairing.

**Impact:** The Gateway and Matching Engine operate on entirely separate physical cache-line pairs, allowing both cores to run at maximum silicon speed without invalidating each other's cache.

---

## 4. Direct Buffer Parsing

### The Problem
Traditional networking applications read bytes into a buffer, deserialize them, and copy the data into application-level structs. Copying memory is expensive, and branching logic to parse fields destroys CPU branch prediction.

### The Implementation
The Trading Simulator sends binary OUCH payloads. Because both the client and server use the same struct packing, the Gateway avoids field-by-field deserialization: it `memcpy`s the bytes into a properly-aligned `OuchEnterOrder` and reads fields directly (`gateway/tcp_server.h`).
```cpp
// Read directly from the kernel into userspace
ssize_t n = read(fd, buffer, sizeof(buffer));

// memcpy into a correctly-aligned struct (NOT a reinterpret_cast).
OuchEnterOrder req;
std::memcpy(&req, buffer + read_pos, sizeof(OuchEnterOrder));
```
**Impact:** The parse is a single 81-byte `memcpy` into an aligned struct, then direct field access — no per-field deserialization. We `memcpy` rather than `reinterpret_cast<OuchEnterOrder*>(buffer)` on purpose: casting a `char*` into a packed-struct pointer is a strict-aliasing and alignment violation (UB), while the compiler routinely elides the `memcpy` into the same loads a cast would emit. This assumes identical struct layout, packing, and endianness between producer and consumer.

---

## 5. Edge-Triggered `epoll` Exhaustion

### The Problem
The Gateway uses `epoll` to monitor thousands of TCP sockets. In Level-Triggered mode, `epoll` constantly wakes the thread as long as there is data, leading to repeated readiness notifications and additional event-processing overhead. In Edge-Triggered mode (`EPOLLET`), the kernel only notifies the application once when new data arrives. If the application doesn't read *all* the data, the thread will stall forever.

### The Implementation
When `epoll_wait` fires, the Gateway enters a `while(true)` loop on that specific socket:
```cpp
while (true) {
    int bytes = read(fd, buffer, sizeof(buffer));
    if (bytes == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break; // Kernel buffer is completely empty
        }
    }
    // Process bytes
}
```
**Impact:** By combining `EPOLLET` with non-blocking sockets (`O_NONBLOCK`), the Gateway guarantees it drains every single byte from the kernel buffer in one userspace trip, minimizing the number of expensive `epoll_wait` syscalls.

> Note: edge-triggered applies to the **client** fds only. The **listen** fd is registered plain level-triggered (`EPOLLIN`, `gateway/tcp_server.h`) on purpose — a level-triggered listener with a single `accept()` per wakeup is correct and simpler than the ET equivalent, which would require an `accept()` drain loop. This is intentional, not an inconsistency.

---

## 6. `SO_REUSEPORT` Sharding

### The Problem
On the benchmark hardware used for this project, a single `epoll`-based ingress thread saturated near 4M msgs/sec. Attempting to push 10M msgs/sec into a single thread overwhelms the ingress path and saturates the TCP receive buffers, resulting in massive queueing delay.

### The Implementation
The system employs a Thread-per-Shard model utilizing the Linux `SO_REUSEPORT` socket option. 
```cpp
int opt = 1;
setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
```
Multiple Gateway threads bind to the exact same IP and Port. The Linux kernel uses a native hash of the incoming connection's 4-tuple (Source IP, Source Port, Dest IP, Dest Port) to deterministically route each TCP stream to a specific Gateway thread.

**Impact:** This design completely bypasses the need for a dedicated acceptor thread and avoids userspace connection handoffs. Scaling to 4 parallel Gateway threads eliminated the queueing delay and restored end-to-end latency to microsecond scales under 10M msgs/sec loads.

---

## 7. Cycle-Accurate Telemetry

### The Problem
Standard timing APIs (`std::chrono::high_resolution_clock`, `clock_gettime()`) invoke the vDSO or the kernel, introducing measurable observer overhead relative to the sub-microsecond events they are trying to measure. 

### The Implementation
The system bypasses software abstractions entirely and reads the CPU's internal Time Stamp Counter (TSC) using hardware intrinsics. It uses `__rdtscp` (not `__rdtsc`) because `__rdtscp` waits for prior instructions to retire before reading the counter — it already serialises against prior loads, so no separate `_mm_lfence()` is needed (`core/timer.h`):

```cpp
inline uint64_t get_tsc() {
    unsigned int dummy;
    return __rdtscp(&dummy);
}
```
Four such timestamps ride on the wire in `OuchEnterOrder` (`t1_exchange_send` … `t4_network_deq`); the Gateway captures a fifth (`t5`) at ingress the moment `read()` returns. The Matching Engine separately pairs a per-task `ingress_tsc` with a `get_tsc()` at match completion — a five-point wire decomposition plus the engine's own execution measurement (see [`telemetry.md`](./telemetry.md)).

**Impact:** This enables low-overhead, cycle-accurate latency decomposition across the ecosystem.

### Example Attribution

The gateway maintains per-stage cycle accumulators (`total_read_cycles`, `total_decode_cycles`, `total_validation_cycles`, `total_enqueue_cycles`) exported in the shutdown stats and the `/dev/shm` region. Concrete per-stage figures are **`TODO(measure)`** — the capacity matrix has not been re-run on reference hardware since the Phase-0/1/3 fixes (see [`benchmarks.md`](./benchmarks.md)); the earlier hardcoded numbers were removed rather than carried forward unverified.
