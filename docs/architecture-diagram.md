# System Architecture — CLI Diagram (multi-firm + private-ack loop)

The current architecture on `feat/private-ack-exchange`: **N genuinely-different
trading firms** competing on one exchange, a **private OUCH ack loop** (the
exchange writes execution reports back on each firm's TCP session), **ack-driven
per-firm position/PnL**, and **per-firm monitoring** the Python TUI reads alongside
the exchange. The same diagram is embedded in the study tracker (`study_tracker.py`,
press `a`).

## Overview

**Three tiers, one closed loop.** N `trading_firm` processes (all the *same*
`LocalExchangeConnector`, made distinct only by env) send OUCH orders to the
exchange; the exchange matches them and — new on this branch — writes an
`OuchExecutionReport` back on each firm's own socket; each firm applies its
confirmed fills to an ack-driven position/PnL and publishes them to a per-firm
`/dev/shm/firm_stats_<FIRM_ID>` seqlock region; the Python monitor reads every
firm's region plus the exchange's, and shows one TUI panel per participant.

**What makes firms different is config, not code:**
- `FIRM_ID` → stamped in `req.firm`; keys the firm's `/dev/shm/firm_stats_<ID>`.
- `TOKEN_BASE` → the firm's order-token slice `[k*SLICE, (k+1)*SLICE)`,
  `SLICE = MAX_CLIENT_ORDERS(50M) / MAX_FIRMS(16)` — so firms never collide in the
  exchange's `SessionManager` (keyed by order token).
- `STRATEGY` → `maker` (posts two-sided quotes) or `taker` (crosses the touch on a
  strong signal), selected via the `IStrategy` interface.

## Full-system diagram

```text
  N TRADING FIRMS  (hft-trading-firm; one process each, distinct by env only)
    FIRM_ID     identity + keys /dev/shm/firm_stats_<FIRM_ID>
    TOKEN_BASE  order-token slice [k*SLICE, (k+1)*SLICE), SLICE = 50M/16
    STRATEGY    maker | taker  (IStrategy)        [same LocalExchangeConnector]

    Firm A (maker)              Firm B (taker)          ...  up to MAX_FIRMS=16
    signal->strategy->risk      signal->strategy->risk
      ->execution(tracker)        ->execution(tracker)
    connector                   connector
      RX acks -> ack-driven        RX acks -> ack-driven
        position / realized PnL      position / realized PnL
      -> FirmStatsRegion (seqlock) -> FirmStatsRegion (seqlock)
         /dev/shm/firm_stats_A        /dev/shm/firm_stats_B

         ^   |                       ^   |                    ^
    acks |   | orders           acks |   | orders       ITCH  |  market data
   (OUCH)|   |(OUCH, TCP :9091)(OUCH)|   |(OUCH)         (UDP) |  (all firms)
         |   v                       |   v                     |
  ====================== EXCHANGE (hft_engine) ========================
   GATEWAY   epoll + SO_REUSEPORT   (per-worker client fds; core map in config.env)
     IN :  read -> decode OUCH -> pre-trade risk -> route by instrument -> ENGINE
     OUT:  [NEW] drain ack_queues[shard][worker]; write OuchExecutionReport back
           to the OWNING client fd  (non-blocking send + EPOLLOUT backpressure)
                    |  route                          ^ acks
                    v                                 | (both sides of each fill,
   ENGINE x4 shards  (price-time matching)            |  routed by order->worker_id)
     on each fill:  -> ack_queues[shard][worker] (OutboundAck) ---+  [NEW]
                    -> mkt_data_q  --> PUBLISHER --> ITCH multicast (UDP) -> firms
                    -> drop_copy_q --> ORDER MANAGER --> order_audit.log
                                                     + /dev/shm/hft_stats (seqlock)

  outputs:   order_audit.log      /dev/shm/hft_stats      /dev/shm/firm_stats_<ID>
                  |                       |                        |
  ====================== PYTHON MONITORING (monitoring/) ==============
   feeds/audit_reader   feeds/stats_reader   feeds/firm_stats_reader.discover()
     (fills/rejects)     (exchange live)       (enumerates ALL firm regions)
                   \            |            /
                    v           v           v
                        tui/app.py
        [ Exchange panel ]  +  [ Firm A ]  +  [ Firm B ]  +  ...
        queue depths,          position, realized PnL, in_flight,
        cycle attribution      per-stage cyc/tick, kill-switch
```

The **private-ack loop** is the new closed cycle:
`firm order -> exchange match -> OuchExecutionReport back on the firm's socket ->
firm applies confirmed fill -> firm_stats region -> Python panel`.

## Exchange internals (unchanged core)

Seven pinned real-time threads, cores from `config.env` (gateway workers
`GATEWAY_CORES` 1,3,5,7; engine shards `ENGINE_CORES` 2,4,6,8; publisher +
order-manager `AUX_CORES` 0,9). Ingress uses one SPSC `EngineTask` queue per
`[shard][worker]`; the new egress mirrors it with one SPSC `OutboundAck` queue per
`[shard][worker]` (engine shard = sole producer, gateway worker = sole consumer).

## Known limitations

Identified during implementation/review. None break the delivered scope; each is a
clear next increment.

**Correctness / risk**
1. **Risk position-limit lag.** `RiskManager::check_order` gates on *confirmed*
   position, which lags fills, so a fast taker can blow through its cap (observed
   position 104k vs a 10k limit). Fix: gate on `confirmed + in_flight` (pending
   exposure) — `in_flight` is already published for exactly this.
2. **Maker `in_flight` overcounts.** The firm consumes only `'E'` (fill) acks; the
   exchange does not yet send NEW/CANCEL/REJECT acks (the private-ack plan's
   deferred "second pass"), so canceled/replaced maker quotes never decrement
   `in_flight` and it grows without bound.
3. **Order-tracker capacity.** `order_tracker` is a fixed 1M-slot array indexed by
   `internal_order_id`, and the strategy's counter grows unbounded — past ~1M
   orders/firm, fills log "unknown Order ID". Long runs need slot reclaim/wrap.

**Integration / operational**
4. **Ack channel drops non-reading clients.** Close-on-overflow disconnects any
   client that never reads its acks (the `liquidity`/`tester` tools). So
   `run_sharding.sh` / `multi_firm_demo.sh` break against an ack-enabled build —
   which is why this work stays on a branch, unmerged from `main`.
5. **Token partition is a convention, not cross-firm enforced.** Each firm
   validates its own slice fits under `MAX_CLIENT_ORDERS`, but nothing stops two
   firms being launched with the same `TOKEN_BASE`; correct non-overlapping
   assignment is the launcher's/operator's responsibility.
6. **`FIRM_ID` is not recorded in the audit.** The exchange doesn't put the firm id
   in drop copies, so per-firm *audit* attribution leans on token ranges (which the
   seed tools muddy). Clean per-firm truth is each firm's own `/dev/shm` region.

**Test coverage**
7. **EPOLLOUT backpressure path is untested.** The gateway's partial-send/`EPOLLOUT`
   branch isn't exercised by a unit test (a socketpair can't easily fill the send
   buffer); it's reviewed by inspection only.

**Pre-existing (not from this work)**
8. **Benchmark numbers remain `TODO(measure)`** — the latency/throughput matrix is
   unmeasured on isolated hardware (see `benchmarks.md`).
9. **Multicast loopback** — firms + the `market_maker` seed rely on ITCH multicast
   (`239.255.0.1:12345`); some boxes need `sudo ip route add 239.0.0.0/8 dev lo` or
   makers idle on an empty book.

## Running & observing it

Build both sides (firm needs no OpenSSL/curl with `WITH_CRYPTO=OFF`):
```bash
cmake -S hft_engine       -B build-eng  -DCMAKE_BUILD_TYPE=Release && cmake --build build-eng  -j$(nproc)
cmake -S hft-trading-firm -B build-firm -DCMAKE_BUILD_TYPE=Release && cmake --build build-firm -j$(nproc)
```

**End-to-end run (prints evidence).** `NUM_FIRMS` (default 2, max `MAX_FIRMS=16`)
controls how many firms; the launcher auto-assigns `FIRM_ID` (A,B,C,…), a disjoint
`TOKEN_BASE = k*SLICE`, and an alternating `maker`/`taker` strategy per firm, seeds
the book, drives flow, and prints per-firm evidence:
```bash
BIN=build-eng FIRM_BIN=build-firm ./scripts/multi_firm_run.sh                 # 2 firms: A maker, B taker
NUM_FIRMS=4 BIN=build-eng FIRM_BIN=build-firm ./scripts/multi_firm_run.sh     # A,B,C,D (maker/taker alternating)
RUN_SECONDS=120 NUM_FIRMS=6 BIN=build-eng FIRM_BIN=build-firm ./scripts/multi_firm_run.sh   # keep alive for the TUI
```
Verified: `NUM_FIRMS=4` → 4 firms, each confined to its own 3.125M-wide token
slice (non-overlapping: PASS), all crossing into confirmed fills with per-firm
position/PnL moving.

**Watch the TUI (exchange + every firm), live:** with a long `RUN_SECONDS` run
going in one terminal, in another terminal:
```bash
python3 -m monitoring.tui.app        # needs `rich`; Exchange panel + one panel per /dev/shm/firm_stats_*
```
The reader's `discover()` enumerates all `firm_stats_*` regions, so every running
firm gets a panel automatically — no matter how many `NUM_FIRMS` you launched.

**Launching firms by hand** (if you want non-alternating strategies or custom ids),
seed the book first so makers can quote:
```bash
build-eng/exchange &                       # or via the launcher's exchange
build-eng/market_maker &                   # seed a standing two-sided book
S=3125000   # = TOKEN_SLICE (50M/16)
FIRM_ID=A TOKEN_BASE=$((0*S)) STRATEGY=maker build-firm/trading_firm local &
FIRM_ID=B TOKEN_BASE=$((1*S)) STRATEGY=taker build-firm/trading_firm local &
FIRM_ID=C TOKEN_BASE=$((2*S)) STRATEGY=maker build-firm/trading_firm local &
# ... each firm gets its own /dev/shm/firm_stats_<ID>; the TUI shows them all
```
Give each firm a **unique `FIRM_ID`** and a **distinct `TOKEN_BASE = k*S`**
(`k = 0..15`); mixing maker/taker is what produces crosses.
