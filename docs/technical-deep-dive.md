# Technical Deep Dive

Achieving 10,000,000 messages per second requires abandoning standard application-level abstractions in favor of mechanical sympathy. This document details the specific C++ techniques used to optimize the hot path, ensuring deterministic sub-microsecond execution times.

---

## 1. Zero Dynamic Allocations (`placement new`)

### The Problem
Standard heap allocations (`new` or `malloc`) rely on the general-purpose userspace allocator. Under high contention, allocator metadata management and synchronization can become a bottleneck. Furthermore, finding contiguous free memory is non-deterministic, meaning an allocation could take 50ns or trigger a 5,000ns page fault.

### The Implementation
The system pre-allocates massive, contiguous byte arrays at startup. During live trading, the hot path strictly uses **Zero Dynamic Allocations**.

When a new order arrives, the Gateway requests a pointer from the lock-free `MemoryPool`. It then uses `placement new` to construct the C++ `Order` object directly into that existing memory address:
```cpp
Order* order = new (pool.allocate()) Order(price, quantity, side);
```
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

## 3. False-Sharing Mitigation (`alignas(64)`)

### The Problem
Modern CPU caches operate in 64-byte chunks called "Cache Lines". In the SPSC queue, the Gateway thread constantly writes to `tail`, and the Matching Engine constantly writes to `head`. 
If `head` and `tail` happen to sit next to each other in memory (within the same 64 bytes), the physical CPU hardware will detect a collision. Every time the Gateway updates `tail`, the CPU will invalidate the Matching Engine's cache line containing `head`, forcing a slow fetch from main memory. This hardware-level thrashing is known as **False Sharing**.

### The Implementation
We force the compiler to physically separate the atomic pointers into distinct cache lines using the `alignas` specifier:
```cpp
alignas(64) std::atomic<size_t> head{0};
alignas(64) std::atomic<size_t> tail{0};
```
**Impact:** The Gateway and Matching Engine operate on entirely separate physical cache lines, allowing both cores to run at maximum silicon speed without invalidating each other's L1 cache.

---

## 4. Direct Buffer Parsing

### The Problem
Traditional networking applications read bytes into a buffer, deserialize them, and copy the data into application-level structs. Copying memory is expensive, and branching logic to parse fields destroys CPU branch prediction.

### The Implementation
The Trading Simulator sends binary OUCH payloads. Because both the client and server use the same struct packing, the Gateway avoids deserialization entirely.
```cpp
// Read directly from the kernel into userspace
int bytes = read(fd, buffer, sizeof(buffer));

// Zero-copy reinterpret
const OuchOrderMessage* msg = reinterpret_cast<const OuchOrderMessage*>(buffer);
```
**Impact:** The parsing phase is reduced to direct field access over an existing memory buffer, eliminating intermediate object construction and memory copies. Data is accessed directly from the receive buffer rather than being copied into temporary parsing structures. This approach assumes identical struct layout, packing, and endianness between producer and consumer.

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
The system bypasses software abstractions entirely and reads the CPU's internal Time Stamp Counter (TSC) using hardware intrinsics. The `__rdtscp` instruction provides the current cycle count, while `_mm_lfence()` (Load Fence) ensures the CPU does not execute the measurement instruction out-of-order via speculation.

```cpp
inline uint64_t get_cycles() {
    uint32_t aux;
    _mm_lfence();
    return __rdtscp(&aux);
}
```
This timestamp is injected directly into the payload of the simulated packet at creation (T0), captured at Gateway ingress (T1), and captured at Matching Engine egress (T2).

**Impact:** This enables low-overhead, cycle-accurate latency decomposition across the ecosystem, definitively proving that the Linux network stack (and not the C++ application) was the primary latency bottleneck.

### Example Attribution

| Stage | Cycles |
|---------|---------:|
| `epoll_wait()` | 1157 |
| `read()` | 124 |
| Decode | 82 |
| Validation | 27 |
| Enqueue | 258 |

This attribution showed that approximately 77% of ingress cost originated in the Linux networking stack, while the application logic accounted for roughly 23%.
