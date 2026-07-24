# System Architecture — CLI Diagram

The current architecture of the HFT Trading-Ecosystem, as wired in
`hft_engine/src/app/exchange.cpp`. The same diagram is embedded in the study
tracker (`python3 study_tracker.py`, press `a`; or `--arch` for stdout).

## Overview

**Three processes.** The C++ **exchange** (`hft_engine`) and the C++
**trading-firm simulator** (`hft-trading-firm`) talk over the wire; the Python
**monitoring** layer is a third, out-of-band process that only *reads* the
exchange's outputs and never touches its hot path.

**Seven pinned real-time threads inside the exchange** (`SCHED_FIFO` prio 99,
`mlockall`, CPU affinity). Core assignments come from `config.env` (single config
source), so pinning tracks the `isolcpus` map instead of drifting from it:

| Thread(s) | Core source (`config.env`) | Job |
| :-- | :-- | :-- |
| Gateway workers ×N | `GATEWAY_CORES` (1,3,5,7) | `SO_REUSEPORT` + epoll; decode OUCH → pre-trade risk → route |
| Engine ×4 | `ENGINE_CORES` (2,4,6,8) | one matching shard each (price-time priority book) |
| Publisher | `AUX_CORES[0]` (0) | ITCH market data → `sendmmsg` multicast |
| OrderManager | `AUX_CORES[1]` (9) | drains reports → mmap audit log + `/dev/shm` counters |
| Main | — | seqlock writer: samples queue depths / cycle attribution every 100ms |

The gateway **dispatcher** thread only spawns the workers and joins them (idle
afterward), so it is deliberately *not* given a reserved core.

**Why every queue is single-producer/single-consumer.** The gateway shards by
`instrument_id % 4`, but there are N workers, so ingress uses **one `EngineTask`
queue per `[shard][worker]` pair** — that is how a lock-free SPSC queue never
gets a second producer. Each engine shard then emits **three** SPSC streams:
`mkt_data_q` (ITCH → Publisher), `drop_copy_q` (NEW/FILL/CANCEL → OrderManager),
and `tsc_q` (ingress→match latency → OrderManager). Pre-trade **rejects** skip
the engine entirely — the worker pushes them straight to a per-worker
`gw_reject_q` into the OrderManager. Orders are allocated from a per-shard
`MemoryPool<Order>` (no `malloc` on the hot path; the pool slot index *is* the
order id).

**The C++ ↔ Python contract is three egress boundaries:** ITCH multicast (UDP),
`order_audit.log` (crash-safe mmap, published via `write_index`), and
`/dev/shm/hft_stats` (seqlock).

## Diagram

```text
  TRADING FIRM SIMULATOR  (hft-trading-firm, separate process)
  strategy -> signal -> risk -> connector -> tcp_client / udp_listener
        |                                              ^
        |  TCP OUCH  order entry :9091      UDP ITCH   |  market data (mcast)
        v                                              |
======================  EXCHANGE  (hft_engine)  ============================

  GATEWAY   TCPServer | N workers | SO_REUSEPORT + epoll
    workers pinned to GATEWAY_CORES (config.env: 1,3,5,7), SCHED_FIFO 99
    per-worker loop:
      epoll_wait -> read -> decode OUCH -> PRE-TRADE RISK
                 -> alloc Order (per-shard MemoryPool)
                 -> route by  instrument_id % 4
      rejects --------------------------------------> gw_reject_q --> OrderMgr

    EngineTask  (SPSC, one queue per [shard][worker])
        |            |            |            |
        v            v            v            v
    +--------+  +--------+  +--------+  +--------+
    |ENGINE 0|  |ENGINE 1|  |ENGINE 2|  |ENGINE 3|   4 shards, 1 thread each
    | order- |  |        |  |        |  |        |   ENGINE_CORES (2,4,6,8)
    | book   |  |        |  |        |  |        |   SCHED_FIFO prio 99
    |price-  |  |        |  |        |  |        |   pinned + mlockall
    | time   |  |        |  |        |  |        |
    +--------+  +--------+  +--------+  +--------+

    each engine shard emits 3 SPSC streams:
      mkt_data_q   (ItchMessage)          --> PUBLISHER
      drop_copy_q  (NEW/FILL/PART/CANCEL) --> ORDER MANAGER
      tsc_q        (ingress->match tsc)   --> ORDER MANAGER

  PUBLISHER  [AUX_CORES[0]]        ORDER MANAGER  [AUX_CORES[1]]
    drain mkt_data_q                 drain drop_copy + reject + tsc
    -> sendmmsg                      -> mmap AUDIT LOG (crash-safe, write_index)
       ITCH multicast (UDP)          -> /dev/shm per-shard counters

  MAIN  [stats sampler | single seqlock writer | every 100ms]
    queue depths | pool high-water | gateway cycle attribution
    -> /dev/shm/hft_stats
  (gateway dispatcher thread only spawns+joins workers; idle, no reserved core)

        |  ITCH mcast          |  order_audit.log      |  /dev/shm/hft_stats
        v                      v                       v
======================  PYTHON MONITORING  (monitoring/)  ==================

  feeds/multicast.py      feeds/audit_reader.py     feeds/stats_reader.py
        |                        |                         |
        v                        v                         v
  core/orderbook.py         fills / rejects           live counters
   (rebuild book)                |                    (seqlock read)
        |                        v                         |
        +-----------> core/metrics.py + health.py <--------+
                             |
                             v
                       tui/app.py   (rich live dashboard)
  orchestrator.py launches the engine and waits for the READY line
```

See [`architecture.md`](./architecture.md) for the design rationale and
[`benchmark-setup.md`](./benchmark-setup.md) for how the core map lines up with
`isolcpus`.
