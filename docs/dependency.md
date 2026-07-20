# Capability → Implementation → Dependency Map

**Reconciled with the code at v1.0.0** (after the implementation plan and the D2–D5 fixes).
This is a *current-state* map, not a plan. If you change what the engine exposes or add a
Python module, update this file in the same commit — it was allowed to drift once already
and spent a release describing a system that no longer existed.

**Sources:** [`cpp-prep-for-python-monitoring.md`](./cpp-prep-for-python-monitoring.md) (§ numbers, interface
detail) · [`review-findings.md`](./review-findings.md) (the A/B/C findings, all resolved) ·
[`implementation-plan.md`](./implementation-plan.md) (phase order; **this doc's module names supersede its
Phase 5 sketch**) · [`known-issues.md`](./known-issues.md) (earlier audit, all resolved) ·
[`v1.0.0-defects.md`](./v1.0.0-defects.md) (defects found after the tag).

**Engine state:** safe to monitor **and** safe to send orders to. The canonical symbol scheme,
the stats region, the TSC anchor and the launch barrier all shipped. The one interface still
unbuilt is **I7** (control socket), which has never been specified.

**Notation:** `→` = imports/depends on. `←` = *fed by at runtime* (data-flow, not an import edge; does not
count against the tier rule).

---

## 1. C++ ↔ Python boundary interfaces (the "connections")

Every Python↔C++ dependency below references one of these taps by ID.

| ID | Interface | Produced by | Wire/contract | Consumable by Python now? |
|---|---|---|---|---|
| **I1** | ITCH multicast `239.255.0.1:12345` | engine (Publisher) | `ItchMessage` 34B, packed | ✅ yes (§2 sets TTL/LOOP/IF) — one message per datagram, no gap detection |
| **I2** | `order_audit.log` (mmap) | engine (OrderManager) | `AuditLogHeader` + `OrderLogEntry[]` | ✅ yes (§5) |
| **I3** | TCP gateway `:9091` | engine (gateway) | OUCH in; Python **produces** orders | ✅ yes — one canonical symbol scheme, see below |
| **I4** | `/dev/shm/hft_stats` (mmap seqlock) | engine | `HftStatsRegion` | ✅ yes (§4) |
| **I5** | TSC→epoch anchor | engine | `TscAnchor{tsc, unix_ns, cycles_per_ns}` | ✅ yes (§3), refreshed while running |
| **I6** | order file (`orders.bin`) | engine tools (`generate_pcap` → `replay`) | **raw `OuchEnterOrder[]`, no framing** | ✅ yes — readable *and* semantically valid |
| **I7** | control socket (kill/flatten/params) | engine | command msgs | ✖ **not built, not specified** |
| **I8** | launch barrier (PID file, `READY` line, heartbeat) | engine | stdout + PID file + I4 field | ✅ yes (§7) |

> **I4 and I5 are one mmap, not two taps.** Per prep §3/§4 the `TscAnchor` lives *inside* `HftStatsRegion`.
> They shipped together, and `clock.py` therefore reads the **same region** as `stats_reader.py`.
> Kept as separate IDs only because different features depend on different halves.
>
> The anchor is **re-published roughly every 100ms**, not written once at startup. A single startup
> calibration left a rate error that grew without bound (defect D2), so a reader must expect
> `cycles_per_ns` to change slightly between snapshots rather than treating it as constant.

> **I6 is not pcap.** The tool name is a misnomer. `generate_pcap` writes `out.write(&req, sizeof(req))`
> in a loop — no pcap global header, no per-packet records, no link-layer frames. It is a flat
> `OuchEnterOrder[]` array. **Consequence: no Python pcap module is needed** — `wire.py` already decodes
> `OuchEnterOrder`, so reading `orders.bin` is one `struct.iter_unpack`.
>
> The file's symbols are canonical (`STK00000`–`STK00003`) and decode correctly. Replaying it produces
> real fills — but only over **multiple connections**. Client identity is per connection and self-trade
> prevention rejects an order crossing its own client's resting order, so a crossing workload sent down
> one socket can only ever reject. `replay` deals orders round-robin over `REPLAY_CONNECTIONS` sockets
> (default 4) for exactly this reason; a Python injector aiming for fills must do the same.

> **The symbol contract (I3).** The gateway accepts exactly one scheme: `STK` followed by five ASCII
> digits, `STK00000`–`STK00255`, capped by `MAX_INSTRUMENTS = 256`. The encoder and decoder live next to
> the structs in `protocol/messages.h` so a client and the gateway cannot disagree — any producer,
> Python included, should mirror that pair rather than hand-rolling a literal.

> **I7 vs prep §7 are different things.** Interface I7 is the *control socket* (unspecified anywhere; needs
> a new §8 in the prep doc). Prep **§7** is the *launch barrier*, tracked here as **I8**. The numbering
> collision is unfortunate; the IDs are the authority.

---

## 2. Feature matrix

Status: ✅ built · ◑ partial · ✖ not done · ⛔ out-of-scope.

Python scope at v1.0.0 is the monitoring path: tiers 0–1 plus `core/orderbook`, `core/metrics`,
`health`, `orchestrator` and `tui`. The backtest, replay and signals tiers are deliberately
not built — they are design targets kept here so the dependency edges stay documented.

| Feature | Owner | Status | C++ side: engine/firm + consumable? | Python module(s) | Depends on |
|---|---|---|---|---|---|
| Protocol decode | Py | ✅ | engine defines structs — consumable ✅ | `wire.py` | *(base)* → I1,I2,I3,I6 layouts |
| Feed handler (parse ITCH) | Both | ✅ | engine ✅ | `feeds/multicast.py` | → `wire`,`config`, **I1** |
| Drop-copy reader | Both | ✅ | engine (audit) — consumable ✅ | `feeds/audit_reader.py` | → `wire`,`config`, **I2** |
| Order-book build (L2) | Both | ✅ | engine — consumable ✅ (I1 deltas) | `core/orderbook.py` | → `wire`,`models`; ← feeds |
| Market-data capture | Py | ✖ | — | `capture.py` | → `feeds/multicast` (I1) |
| Reference/symbology data | Py | ✖ | scheme exists (`STK#####`); no Py module yet | `refdata.py` | → `config` |
| Historical tick store | Py | ✖ | — | `dataset.py` | → `wire`; ← `capture`, **I2** |
| Order entry (produce orders) | Both | ✅ C++ / ✖ Py | engine gateway ✅ | `replay/order_injector.py` | → `wire`, **I3** |
| Order-file read (`orders.bin`) | Py | ✅ | engine tools I6 ✅ | *(none — `wire.py` covers it)* | → `wire`, **I6** |
| Deterministic replay (orders→engine) | Both | ✅ C++ / ✖ Py | engine (`replay`) — produces fills over ≥2 connections | `replay/order_injector.py` | → `wire`, **I3**+**I6** |
| Session replay (time machine viz) | Py | ✖ | — | `replay/session_replay.py` | → `dataset`,`orderbook`,`clock` (**I5**) |
| Backtest engine | Py | ✖ | uses real engine as fill authority | `backtest/engine.py` | → `dataset`,`strategy`,`fill_model`,`portfolio`,`risk`,`clock` (**I5**) |
| Fill model | Py | ✖ | (a) pure-Py / (b) real engine I3+I2 | `backtest/fill_model.py` | → `orderbook` **or** `order_injector`+`audit_reader` |
| Strategy engine | Both | ✅ firm / ✖ Py | firm (C++) — not consumed; Py re-impl | `backtest/strategy.py` | → `models`,`orderbook` |
| Signal/alpha | Py | ✖ | — | `signals/*.py` | → `orderbook`,`dataset` |
| Pre-trade risk (sim mirror) | Both | ✅ engine / ✖ Py | engine (hot path) — limits **hardcoded**, see note | `backtest/risk.py` | → `config` (**§6**) |
| Position / exposure | Py | ✖ | — | `portfolio.py` | → `models`; ← `audit_reader` |
| PnL & attribution | Py | ✖ | — | `backtest/report.py`,`portfolio.py` | → `portfolio`,`metrics` |
| TCA / analytics | Py | ✖ | — | `backtest/report.py` | → `portfolio`,`metrics` |
| Live dashboard (book/tape) | Py | ✅ | — | `tui/app.py` | → `feeds`,`orderbook`,`metrics`,`health` |
| Latency monitoring (t1–t4) | Both | ◑ measured / ◑ surfaced | engine+gateway — cycle attribution in I4 | `core/metrics.py` | → **I4** |
| System health (queues/drops) | Both | ✅ | engine ✅ via I4 | `feeds/stats_reader.py`,`health.py` | → `wire`, **I4** |
| Metrics/rates | Py | ✅ | — | `core/metrics.py` | → `feeds`,`orderbook`,`clock` |
| Prometheus/alerting | Py | ✖ | — | `exporters/prometheus.py` | → `metrics`,`health` |
| Structured logging | Py | ✖ | — | `logging_.py` | → `config` |
| Wall-clock timestamps | Both | ✅ | engine publishes the anchor in I4/I5 | `clock.py` | → **I5** |
| Kill switch / flatten / live params | Both | ✖ | engine — not built, **not specified** | `control/command.py` | → **I7** |
| Orchestration (start/stop) | Both | ✅ | engine prints `READY` + PID file (§7) | `orchestrator.py` | → `config`, **I8** |
| Central config | Both | ✅ | `config.env`, engine `getenv`s it (§6) | `config.py` | *(base)* |
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

### Gap detection still has zero C++ support

`tracking_number` appears exactly **once** in the tree — the struct definition in
`hft_engine/src/protocol/messages.h` (the firm's duplicate header was removed when the wire schema
was unified). Nothing ever writes it. There is no per-shard sequence to detect a gap against, so
this row is ✖, not ◑.

This is not academic. I1 delivers **one message per datagram**, and a burst can overflow the socket
receive buffer; measured loss on the development box is ~0.8% under a full-cross burst, bounded by
`net.core.rmem_max`. A consumer cannot tell that it happened. `core/orderbook.py` compensates
defensively — it repairs a crossed book and ages out orders whose removal never arrived (defect
D4) — but that is damage control, not detection.

**Needs a new prep-doc item:** populate a per-shard monotonic sequence. Note `uint16_t` wraps ~150×/sec at
10M msgs/sec, so a reader must handle wrap — or the field gets widened, which is a **layout change** and
therefore belongs with §1's protocol versioning, not bolted on after.

### The risk mirror still has no shared source of truth

`backtest/risk.py` is listed as depending on `config`, but the engine's limits are **still
constructor-hardcoded** in `gateway/risk_engine.h` (`max_qty`, `max_price`, `max_notional`) and are
**not** in `config.env`. Either add them, or the Python mirror drifts from the engine by construction
the first time anyone tunes a limit.

---

## 3. Python module dependency graph (base → dependent)

Tier rule: a module only **imports** (`→`) from strictly lower tiers. Runtime feeds (`←`) may cross sideways.
Modules marked *(not built)* are design targets; their edges are recorded so the graph stays sound
when someone builds them.

| Tier | Module | → imports (Py) | C++ interface |
|---|---|---|---|
| **0 — base** | `wire.py` | — | I1,I2,I3,I6 layouts |
| **0 — base** | `config.py` | — | — |
| **0 — base** | `models.py` | — | — |
| **1** | `feeds/multicast.py` | wire, config | **I1** ✅ |
| **1** | `feeds/audit_reader.py` | wire, config | **I2** ✅ |
| **1** | `feeds/stats_reader.py` | wire, config | **I4** ✅ |
| **1** | `clock.py` | wire | **I5** ✅ *(same region as stats_reader)* |
| **2** | `core/orderbook.py` | wire, models | ← multicast |
| **2** | `capture.py` *(not built)* | feeds/multicast | I1 |
| **2** | `dataset.py` *(not built)* | wire | ← capture, **I2** |
| **2** | `replay/order_injector.py` *(not built)* | wire | **I3** ✅ (produces), reads **I6** via wire |
| **3** | `core/tape.py` *(not built)* | orderbook | — |
| **3** | `core/metrics.py` | clock | (I4) |
| **3** | `replay/session_replay.py` *(not built)* | dataset, orderbook, clock | **I5** ✅ |
| **3** | `backtest/fill_model.py` *(not built)* | orderbook **or** order_injector+audit_reader | (I3+I2 in real-engine mode) |
| **4** | `health.py` | core/metrics | **I4** ✅ |
| **4** | `portfolio.py` *(not built)* | models | ← audit_reader (I2) |
| **4** | `backtest/risk.py` *(not built)* | config | — |
| **4** | `backtest/strategy.py` *(not built)* | models, orderbook | — |
| **5** | `backtest/engine.py` *(not built)* | dataset, strategy, fill_model, portfolio, risk, clock | (I3/I2 via fill_model) |
| **5** | `backtest/report.py` *(not built)* | portfolio, metrics | — |
| **5** | `tui/app.py` | config, clock, feeds/*, core/metrics, core/orderbook, wire, health | — |
| **5** | `exporters/prometheus.py` *(not built)* | metrics, health | — |
| **5** | `control/command.py` *(not built)* | config | **I7** ✖ |
| **5** | `orchestrator.py` | config, clock, feeds/stats_reader *(heartbeat)* | **I8** ✅ *(heartbeat field lives in I4)* |

**`models.py`** (load-bearing for tiers 2–5, so pinning it down): the *domain* types — `Order`, `Fill`,
`BookLevel`, `Position`, and the `Side`/`OrderState` enums mirroring `protocol/messages.h`. Plain dataclasses,
no I/O, no imports. **`wire.py` does not import it:** wire's decoders return flat wire-records (namedtuples
defined in `wire.py` itself, field-for-field mirrors of the C structs); tier-2+ modules build domain models
from those records (`orderbook` builds `BookLevel`s, `portfolio` builds `Position`s). Tier 0 therefore has
no internal edges.

### What is built, and what is left

- **Built and working:** `wire`, `config`, `models`, `clock`, all three `feeds/*`, `core/orderbook`,
  `core/metrics`, `health`, `orchestrator`, `tui`. Every one carries a self-test; run them with
  `python3 -m monitoring.run_tests`.
- **Not built, nothing blocking it:** `capture`, `dataset`, `refdata`, `portfolio`, `recon`,
  `core/tape`, `replay/*`, `backtest/*`, `signals/*`, `exporters/*`, `logging_`. Every C++ interface
  these need (I1, I2, I3, I5, I6) now exists — they are simply unwritten, and were scoped out of v1.
- **Blocked on an unwritten spec:** `control/command` (I7), and gap detection (needs a wire sequence
  number, which is a layout change).
- **Blocked on a config decision:** `backtest/risk` cannot faithfully mirror the engine until the risk
  limits move into `config.env` — see the note above.

---

## ITCH decoder note

With `internal_id` equal to the pool slot index, ITCH order-reference ids **recycle** (an id is reused
after its order leaves the book) and are unique per `(shard, id)` rather than globally monotonic. Standard
ITCH semantics, but `core/orderbook.py` must treat an `A` (add) as starting a fresh order for that id.

Because the feed can silently drop messages (see *Gap detection*), a reconstructed book also has to
assume it will sometimes be wrong: `core/orderbook.py` repairs a crossed book — impossible in reality,
so proof of a stranded order — and ages out orders untouched past a TTL.
