# Plan: firm-side monitoring (symmetric to the exchange)

Today the Python monitoring layer is **exchange-only**: `feeds/stats_reader.py`
reads the exchange's `HftStatsRegion` in `/dev/shm`, `feeds/audit_reader.py` reads
`order_audit.log`, `feeds/multicast.py` reads ITCH. The **firm** exposes nothing —
no shared-memory region, no audit log, no seqlock. It only self-reports cycle
attribution / PnL to **stdout at shutdown** (`main.cpp`), plus the `market_maker`
tool's live `[PnL Report]`. This plan gives the firm the same observability
surface the exchange already has, so a Python panel can watch it live.

## Objective

Publish the firm's live state to a seqlock-protected `/dev/shm/firm_stats` region
and read it from a new Python feed + TUI panel — mirroring the exchange path
almost verbatim. Fields:

- **Position** per instrument (`risk/risk_manager.h` already holds
  `current_positions[1000]`).
- **Realized PnL / cash** (computable from confirmed fills — see the dependency
  below).
- **Orders sent / acked / in-flight** (`LocalExchangeConnector` already counts
  `total_orders_sent`; acked/in-flight need the ack channel).
- **Per-stage cycle attribution** — BookBuilder / Signals / Strategy / Risk /
  Execution (`main.cpp` already accumulates these; today they're local and printed
  only at shutdown).
- **Connector cycles** — serialize / send / enqueue / dequeue
  (`LocalExchangeConnector` already has these atomics).
- **Kill-switch state** (`risk_manager`), plus a **heartbeat** tsc.

## Hard dependency on the private-ack plan (read this first)

The firm today updates `current_positions` in `risk_manager` when it **approves an
order it is about to send** — i.e. on *intent*, not on *confirmed fills*. Its
position/PnL view is therefore optimistic bookkeeping, not truth: it doesn't know
what actually filled, partially filled, or rejected, because the exchange never
acks it (`docs/private-ack-plan.md`).

So accurate firm monitoring **requires the ack channel first**. The correct
accounting is:

- position/PnL move on **confirmed executions** received via the ack channel
  (`OuchExecutionReport` on the firm's `tcp_rx_thread`), matched to the firm's own
  `order_token`;
- the pre-send `risk_manager` update becomes a **pending/exposure** figure (orders
  sent but not yet acked), *not* a second position update.

⚠️ **Do not double-count.** If you keep the intent-based `current_positions +=`
*and* add an ack-based update, every filled order counts twice. Pick one home for
"position" (confirmed/ack-driven) and repurpose the pre-send number as
"pending exposure."

## Design (mirror the exchange's stats region)

Reuse the exact pattern from `core/stats_region.h` + the exchange's
`map_stats_region` / seqlock writer in `app/exchange.cpp`:

```
struct FirmStatsRegion {          // alignas(64), seqlock-protected sampled block
    std::atomic<uint64_t> seq;    // even = stable, odd = writer in progress
    // sampled block:
    int64_t  position[MAX_INSTRUMENTS];
    int64_t  realized_pnl;
    uint64_t orders_sent, orders_acked, in_flight;
    uint64_t book_cycles, signal_cycles, strategy_cycles, risk_cycles, exec_cycles, ticks;
    uint64_t serialize_cycles, send_cycles, enqueue_cycles, dequeue_cycles;
    uint32_t kill_switch;
    uint64_t heartbeat_tsc;
};
```

## Step-by-step

**1. Map the region.** Add a `firm_stats.h` that `mmap`s `/dev/shm/firm_stats`
(path from `FIRM_STATS_SHM_PATH`, default `/dev/shm/firm_stats`) — a near-copy of
`map_stats_region`. Add `FIRM_STATS_SHM_PATH` to `config.env`.

> **Per-firm keying (when multiple firms exist).** With
> `docs/multi-firm-plan.md`, each firm process must publish to its **own** region,
> keyed by `FIRM_ID` — e.g. default the path to `/dev/shm/firm_stats_<FIRM_ID>`.
> The Python reader then enumerates the per-firm regions and the TUI shows one
> panel per firm. `FIRM_ID` (introduced by the multi-firm plan) is the join key.

**2. Make the counters reachable by one writer.** The `main.cpp` per-stage cycle
counters are currently locals inside the market-data callback closure; promote
them to a small struct the writer can read. The connector atomics and
`risk_manager` position array are already accessible.

**3. Single seqlock writer.** Simplest coherent choice: the **market-data callback
thread** publishes the sampled block every ~100 ms (it already runs per tick —
gate on elapsed time), so there is exactly one writer and no cross-thread race,
matching the exchange's single-writer discipline. Bump `seq` odd → write fields →
bump even.

**4. Ack-driven position/PnL (depends on the ack plan).** In the firm's
`tcp_rx_thread`, on each `OuchExecutionReport`: look up the firm's order by
`order_token`, apply `executed_shares`/`execution_price` to position + realized
PnL, and increment `orders_acked` / decrement `in_flight`. This replaces the
intent-based position update (see the dependency section).

**5. Python reader + panel.** Add `monitoring/feeds/firm_stats_reader.py` — a
near-copy of `feeds/stats_reader.py`'s seqlock read against `FirmStatsRegion`.
Surface it as a TUI panel in `tui/app.py` alongside the exchange panels
(position, PnL, in-flight, per-stage cycles, kill-switch).

## Testing

- **Unit:** write the region from a tiny driver, read it back with
  `firm_stats_reader.py`, assert round-trip + seqlock consistency (mirror the
  existing stats-reader test).
- **Integration:** run the firm against the exchange (with the ack channel live),
  send crossing flow via `scripts/multi_firm_demo.sh`, and confirm the Python
  panel shows position/PnL moving on **confirmed fills**, `in_flight` draining as
  acks arrive, and cycle attribution updating live.

## Scope & risk

~2–3 hrs *on top of the ack channel*: region struct + map (30m) · promote counters
+ single-writer publish (45m) · ack-driven position/PnL, no double-count (45m,
the correctness risk) · Python reader + TUI panel (45m) · tests (30m). The main
risk is the position double-count — get the "confirmed vs pending" split right.

## Relationship to the private-ack plan

These two plans are designed to compose (see `docs/private-ack-plan.md`):

```
exchange match -> OuchExecutionReport (ack plan)
              -> firm tcp_rx_thread applies fill to position/PnL
              -> firm writes FirmStatsRegion under seqlock (this plan)
              -> monitoring/feeds/firm_stats_reader.py -> TUI panel
```

Order of implementation: **ack channel first**, then firm monitoring — the
monitoring's accuracy depends entirely on the ack channel existing. Building
monitoring first would just surface the same optimistic (wrong) intent-based
numbers the firm prints today.
