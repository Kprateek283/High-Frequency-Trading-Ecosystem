# Capability → Implementation → Dependency Map

**Sources:** [`cpp-prep-for-python-monitoring.md`](./cpp-prep-for-python-monitoring.md) (§ numbers, interface
readiness) · [`review-findings.md`](./review-findings.md) (A1 symbol scheme, open HIGH) ·
[`implementation-plan.md`](./implementation-plan.md) (phase order; **this doc's module names supersede its
Phase 5 sketch**) · [`known-issues.md`](./known-issues.md) (earlier audit).

**Engine state:** safe to **monitor**. **Not** safe to send orders to from a new client until the symbol
scheme (review A1) lands — see the blocker note under I3. `known-issues.md`'s CRITICAL/MED/LOW are all
resolved, but that predates `review-findings.md`, which adds an unresolved **[CRITICAL] A1** and leaves one
**[HIGH]** open (gateway worker threads unpinned / not `SCHED_FIFO`).

**Notation:** `→` = imports/depends on. `←` = *fed by at runtime* (data-flow, not an import edge; does not
count against the tier rule).

---

## 1. C++ ↔ Python boundary interfaces (the "connections")

Every Python↔C++ dependency below references one of these taps by ID.

| ID | Interface | Produced by | Wire/contract | Consumable by Python now? |
|---|---|---|---|---|
| **I1** | ITCH multicast `239.255.0.1:12345` | engine (Publisher) | `ItchMessage` 34B, packed | ◑ **localhost only** until §2 sets TTL/LOOP/IF |
| **I2** | `order_audit.log` (mmap) | engine (OrderManager) | `AuditLogHeader` + `OrderLogEntry[]` | ✅ yes (§5) |
| **I3** | TCP gateway `:9091` | engine (gateway) | OUCH in; Python **produces** orders | ◑ **transport ready, symbol contract broken** — see below |
| **I4** | `/dev/shm/hft_stats` (mmap seqlock) | engine | `HftStatsRegion` | ✖ **needs §4** |
| **I5** | TSC→epoch anchor | engine | `TscAnchor{tsc, unix_ns, cycles_per_ns}` | ✖ **needs §3** |
| **I6** | order file (`orders.bin`) | engine tools (`generate_pcap` → `replay`) | **raw `OuchEnterOrder[]`, no framing** | ✅ readable; ✖ **contents useless** — see below |
| **I7** | control socket (kill/flatten/params) | engine | command msgs | ✖ **not built, not specified** |
| **I8** | launch barrier (PID file, `READY` line, heartbeat) | engine | stdout + PID file + I4 field | ✖ **needs §7** |

> **I4 and I5 are one mmap, not two taps.** Per prep §3/§4 the `TscAnchor` lives *inside* `HftStatsRegion`.
> They ship together in one change, and `clock.py` therefore reads the **same region** as `stats_reader.py`.
> Kept as separate IDs only because different features depend on different halves.

> **I6 is not pcap.** The tool name is a misnomer. `generate_pcap.cpp:48` is
> `out.write(&req, sizeof(req))` in a loop — no pcap global header, no per-packet records, no link-layer
> frames. `replay.cpp` mmaps the file and blasts the bytes straight at the TCP gateway. It is a flat
> `OuchEnterOrder[]` array. **Consequence: no Python pcap module is needed** — `wire.py` already decodes
> `OuchEnterOrder`, so reading `orders.bin` is one `struct.iter_unpack`. *Second* consequence: the file's
> symbols are `AAPL/MSFT/GOOG/AMZN` (`generate_pcap.cpp:20`), which the gateway decodes to `999`
> (`tcp_server.h:244`) and pre-trade risk rejects (`999 >= MAX_INSTRUMENTS`). **Every order in it is
> rejected.** The file is byte-readable and semantically worthless until A1.

> **I3's blocker (review A1).** The gateway accepts exactly one symbol scheme, `STK00000`–`STK00255`.
> Transport-level, I3 is consumable today — a Python injector can connect and get well-formed rejects. But
> any injector that wants **fills** is blocked on A1 landing a canonical symbol scheme. Do not read
> "I3 ✅" as "replay works end to end".

> **I7 vs prep §7 are different things.** Interface I7 is the *control socket* (unspecified anywhere; needs
> a new §8 in the prep doc). Prep **§7** is the *launch barrier*, tracked here as **I8**. The numbering
> collision is unfortunate; the IDs are the authority.

---

## 2. Feature matrix

Status: ✅ built · ◑ partial · ✖ not done · ⛔ out-of-scope.

| Feature | Owner | Status | C++ side: engine/firm + consumable? | Python module(s) | Depends on |
|---|---|---|---|---|---|
| Protocol decode | Py | ✖ | engine defines structs — consumable ✅ | `wire.py` | *(base)* → I1,I2,I3,I6 layouts |
| Feed handler (parse ITCH) | Both | ✅ C++ / ✖ Py | engine — ◑ localhost until §2 | `feeds/multicast.py` | → `wire`,`config`, **I1** |
| Drop-copy reader | Both | ✅ C++ / ✖ Py | engine (audit) — consumable ✅ | `feeds/audit_reader.py` | → `wire`,`config`, **I2** |
| Order-book build (L2) | Both | ✅ engine / ✖ Py | engine — consumable ✅ (I1 deltas) | `core/orderbook.py` | → `wire`,`models`; ← feeds/dataset |
| Market-data capture | Py | ✖ | — | `capture.py` | → `feeds/multicast` (I1) |
| Reference/symbology data | Py | ✖ | **blocked on A1** — no canonical scheme exists | `refdata.py` | → `config` |
| Historical tick store | Py | ✖ | — | `dataset.py` | → `wire`; ← `capture`, **I2** |
| Order entry (produce orders) | Both | ✅ C++ / ✖ Py | engine gateway — ◑ **A1** | `replay/order_injector.py` | → `wire`, **I3** |
| Order-file read (`orders.bin`) | Py | ✖ | engine tools I6 — ✅ readable, ✖ contents | *(none — `wire.py` covers it)* | → `wire`, **I6** |
| Deterministic replay (orders→engine) | Both | ◑ C++ / ✖ Py | engine (`replay`) — **produces 100% rejects until A1** | `replay/order_injector.py` | → `wire`, **I3**+**I6** |
| Session replay (time machine viz) | Py | ✖ | — | `replay/session_replay.py` | → `dataset`,`orderbook`,`clock` (**I5**) |
| Backtest engine | Py | ✖ | uses real engine as fill authority | `backtest/engine.py` | → `dataset`,`strategy`,`fill_model`,`portfolio`,`risk`,`clock` (**I5**) |
| Fill model | Py | ✖ | (a) pure-Py / (b) real engine I3+I2 | `backtest/fill_model.py` | → `orderbook` **or** `order_injector`+`audit_reader` |
| Strategy engine | Both | ✅ firm / ✖ Py | firm (C++) — not consumed; Py re-impl | `backtest/strategy.py` | → `models`,`orderbook` |
| Signal/alpha | Py | ✖ | — | `signals/*.py` | → `orderbook`,`dataset` |
| Pre-trade risk (sim mirror) | Both | ✅ engine / ✖ Py | engine (hot path) — limits **hardcoded**, see note | `backtest/risk.py` | → `config` (**§6**) |
| Position / exposure | Py | ✖ | — | `portfolio.py` | → `models`; ← `audit_reader` |
| PnL & attribution | Py | ✖ | — | `backtest/report.py`,`portfolio.py` | → `portfolio`,`metrics` |
| TCA / analytics | Py | ✖ | — | `backtest/report.py` | → `portfolio`,`metrics` |
| Live dashboard (book/tape) | Py | ✖ | — | `tui/app.py`,`tui/widgets/*` | → `feeds`,`orderbook`,`tape`,`metrics`,`health` |
| Latency monitoring (t1–t4) | Both | ✅ measured / ✖ surfaced | engine+gateway — **not consumable** | `metrics.py`,`tui/widgets/latency.py` | → **I4** |
| System health (queues/drops) | Both | ◑ counted → stderr / ✖ Py | engine — **not consumable** | `feeds/stats_reader.py`,`health.py` | → `wire`, **I4** |
| Metrics/rates | Py | ✖ | — | `core/metrics.py` | → `feeds`,`orderbook`,`clock` |
| Prometheus/alerting | Py | ✖ | — | `exporters/prometheus.py` | → `metrics`,`health` |
| Structured logging | Py | ✖ | — | `logging_.py` | → `config` |
| Wall-clock timestamps | Both | ✖ | engine — `cycles_per_ns` private to `Timer::calibrate`, exported nowhere | `clock.py` | → **I5** |
| Kill switch / flatten / live params | Both | ✖ | engine — not built, **not specified** | `control/command.py` | → **I7** |
| Orchestration (start/stop) | Py | ✖ | engine — **no launch barrier** (§7) | `orchestrator.py` | → `config`, **I8** |
| Central config | Both | ◑ hardcoded/env | engine (§6) | `config.py` | *(base)* |
| Trade capture / booking | Both | ✅ audit / ✖ Py | engine (I2) — consumable ✅ | `portfolio.py` | → `audit_reader` (**I2**) |
| Position reconciliation | Py | ✖ | — | `recon.py` | → `portfolio`,`audit_reader` |
| Low-latency net / IPC | C++ | ✅ | engine — n/a to Py | — | — |
| Immutable audit trail | C++ | ✅ | engine (I2, §5) | `audit_reader` (verifier) | → **I2** |
| Snapshot/gap recovery | Both | ✖ | engine — **no sequence number is ever written**, see note | `feeds/multicast.py` | → **I1** + new prep item |
| SOR / multi-venue | — | ⛔ | single venue | — | — |
| Clearing & settlement | — | ⛔ | no clearing house in sim | — | — |
| Failover / HA | — | ⛔ | single-process research system | — | — |
| Surveillance (spoofing) | — | ⛔ | compliance, later nice-to-have | — | — |
| Regulatory reporting | — | ⛔ | no regulator in sim | — | — |

### Gap detection has zero C++ support

`tracking_number` appears exactly **twice** in the tree — the two struct definitions
(`hft_engine/src/protocol/messages.h:46`, `hft-trading-firm/src/network/messages.h:55`). Nothing ever
writes it. There is no per-shard sequence to detect a gap against, so this row is ✖, not ◑.

**Needs a new prep-doc item:** populate a per-shard monotonic sequence. Note `uint16_t` wraps ~150×/sec at
10M msgs/sec, so a reader must handle wrap — or the field gets widened, which is a **layout change** and
therefore belongs with §1's protocol versioning, not bolted on after.

### The risk mirror has no shared source of truth

`backtest/risk.py` is listed as depending on `config`, but the engine's limits are **constructor-hardcoded**
in `gateway/risk_engine.h` and are **not** among §6's minimum config keys. Either add them to §6, or the
Python mirror drifts from the engine by construction the first time anyone tunes a limit.

---

## 3. Python module dependency graph (base → dependent)

Tier rule: a module only **imports** (`→`) from strictly lower tiers. Runtime feeds (`←`) may cross sideways.

| Tier | Module | → imports (Py) | C++ interface |
|---|---|---|---|
| **0 — base** | `wire.py` | — | I1,I2,I3,I6 layouts |
| **0 — base** | `config.py` | — | — |
| **0 — base** | `models.py` | — | — |
| **1** | `feeds/multicast.py` | wire, config | **I1** ◑ |
| **1** | `feeds/audit_reader.py` | wire, config | **I2** ✅ |
| **1** | `feeds/stats_reader.py` | wire, config | **I4** ✖ |
| **1** | `clock.py` | wire, config | **I5** ✖ *(same region as stats_reader)* |
| **2** | `core/orderbook.py` | wire, models | ← multicast/dataset |
| **2** | `capture.py` | feeds/multicast | I1 |
| **2** | `dataset.py` | wire | ← capture, **I2** |
| **2** | `replay/order_injector.py` | wire | **I3** ◑ (produces), reads **I6** via wire |
| **3** | `core/tape.py` | orderbook | — |
| **3** | `core/metrics.py` | feeds, orderbook, clock | (I4) |
| **3** | `replay/session_replay.py` | dataset, orderbook, clock | **I5** ✖ |
| **3** | `backtest/fill_model.py` | orderbook **or** order_injector+audit_reader | (I3+I2 in real-engine mode) |
| **4** | `health.py` | stats_reader, metrics | **I4** ✖ |
| **4** | `portfolio.py` | models | ← audit_reader (I2) |
| **4** | `backtest/risk.py` | config | — |
| **4** | `backtest/strategy.py` | models, orderbook | — |
| **5** | `backtest/engine.py` | dataset, strategy, fill_model, portfolio, risk, clock | (I3/I2 via fill_model) |
| **5** | `backtest/report.py` | portfolio, metrics | — |
| **5** | `tui/app.py` (+widgets) | feeds, orderbook, tape, metrics, health **or** session_replay | — |
| **5** | `exporters/prometheus.py` | metrics, health | — |
| **5** | `control/command.py` | config | **I7** ✖ |
| **5** | `orchestrator.py` | config, feeds/stats_reader *(heartbeat)* | **I8** ✖ *(heartbeat field lives in I4)* |

**`models.py`** (load-bearing for tiers 2–5, so pinning it down): the *domain* types — `Order`, `Fill`,
`BookLevel`, `Position`, and the `Side`/`OrderState` enums mirroring `protocol/messages.h`. Plain dataclasses,
no I/O, no imports. **`wire.py` does not import it:** wire's decoders return flat wire-records (namedtuples
defined in `wire.py` itself, field-for-field mirrors of the C structs); tier-2+ modules build domain models
from those records (`orderbook` builds `BookLevel`s, `portfolio` builds `Position`s). Tier 0 therefore has
no internal edges.

### What is actually unblocked

- **Fully unblocked:** `wire`, `config`, `models`, `feeds/audit_reader` (I2), `capture`/`dataset` (I2 +
  localhost I1), `portfolio`/`recon` off the audit log, `core/orderbook` fed from `dataset`.
- **Blocked on §3/§4 (one change):** `clock`, `feeds/stats_reader`, `health`, latency surfacing — and
  therefore **`session_replay` and `backtest/engine`, both of which import `clock`**. Backtesting is *not*
  unblocked. Without I5 Python cannot convert a cycle delta to time at all.
- **Blocked on §7 (+ §4 for the heartbeat field):** `orchestrator` (without I8 it is `sleep 2` with extra steps).
- **Blocked on review A1:** anything expecting **fills** rather than rejects — `order_injector` end-to-end,
  `refdata`, and `fill_model`'s real-engine mode.
- **Blocked on an unwritten spec:** `control/command` (I7), gap detection.

---

## ITCH decoder note

With `internal_id` now equal to the pool slot index, ITCH order-reference ids **recycle** (an id is reused
after its order leaves the book) and are unique per `(shard, id)` rather than globally monotonic. Standard
ITCH semantics, but `core/orderbook.py` must treat an `A` (add) as starting a fresh order for that id.
