# Plan: multiple genuinely-different trading firms

Today "multiple firms" does not really exist. `scripts/multi_firm_demo.sh` uses the
engine-side injector *tools* (`liquidity`, `tester`) — dumb order generators with no
strategy. The real firm (`hft-trading-firm`) runs a **single hardcoded strategy**
(`MarketMakerStrategy`) with a hardcoded id (`"HFT1"`, `LocalExchangeConnector.h:151`)
and a per-process token counter. Starting the binary N times yields **correlated
clones that collide** (same firm id, overlapping order tokens), not distinct
competitors. This plan makes N firms that are actually different — separate
identities and separate behaviours — the way real venues see many participants.

## Objective

Run N config-driven firm processes that (a) have distinct, non-colliding
identities on the exchange, and (b) pursue genuinely different strategies (e.g. a
passive market maker vs an aggressive taker), so the matching engine sees real
competition.

## Two independent dimensions of "different"

**A. Identity separation (correctness — required, small).**
Without this, two firms corrupt each other on the exchange:
- **Firm id**: `LocalExchangeConnector.h:151` hardcodes `req.firm = "HFT1"`. Drive it
  from a `FIRM_ID` env var.
- **Order tokens**: the token is `action.internal_order_id`
  (`LocalExchangeConnector.h:137`), a per-process counter starting at 1 — so every
  firm emits `1,2,3…` and the exchange's `SessionManager` (keyed by `order_token`,
  `MAX_CLIENT_ORDERS = 50,000,000`, session_manager.h:28) has firm B overwrite firm
  A's entries. Fix by **partitioning the 50M token space**: `token = TOKEN_BASE +
  internal_order_id`, with `TOKEN_BASE = firm_index * SLICE` and `SLICE =
  MAX_CLIENT_ORDERS / MAX_FIRMS` (e.g. 8 firms → ~6.25M tokens each). Keep the
  per-firm counter below `SLICE`. Set `TOKEN_BASE` from env.
- (Connection `client_id` is already assigned uniquely per connection by the
  gateway, so no change needed there — only the token map collides.)

**B. Strategy diversity (behaviour — the "actually different" part).**
- **Parameterise the existing `MarketMakerStrategy`** (`strategy/market_maker.h`):
  turn the hardcoded `SPREAD_MARGIN`, `IMBALANCE_THRESHOLD`, quote size (`100`), skew
  magnitude, and target instrument(s) into ctor params loaded from env. Two market
  makers with different spread/skew already quote differently and cross each other.
- **Add a second strategy type — an aggressive Taker/Momentum firm**: instead of
  quoting passively, it *crosses the spread* (sends marketable orders) when the
  signal is strong (`|imbalance|` or micro-price drift beyond a threshold from
  `SignalEngine`). Maker-vs-taker is the classic real-world "fight": makers post
  liquidity, takers lift it → guaranteed, strategy-driven fills.
- Select the strategy per instance via a `STRATEGY` env (`maker` | `taker`) behind a
  minimal strategy interface (two implementations — an interface is justified once
  there are two, not before). `main.cpp` instantiates the chosen one.

## Design decision: N processes, not N threads

Run each firm as its **own process** (`FIRM_ID=A STRATEGY=maker ./trading_firm`, …),
config-driven by env. This mirrors reality (firms are separate processes/hosts), is
the smallest change (just parameterise what's hardcoded), keeps each firm's state
isolated, and needs no shared-memory coordination between firms. A single-process
N-thread design would add complexity for no benefit here (YAGNI).

## Step-by-step

**Part A — identity (do first; tiny, correctness-critical).**
1. `FIRM_ID` env → `req.firm` in `LocalExchangeConnector` (replace the `"HFT1"`
   memcpy at all three send sites; pad/truncate to 4 chars).
2. `TOKEN_BASE` env → `token = TOKEN_BASE + internal_order_id` where the token is
   built (`LocalExchangeConnector.h:137` and the two sibling paths). Assert
   `TOKEN_BASE + max_local_counter < MAX_CLIENT_ORDERS`.

**Part B — strategy config.**
3. Give `MarketMakerStrategy` a ctor taking `{spread_margin, imbalance_threshold,
   quote_size, skew, instruments}` and load them from env in `main.cpp` (defaults =
   today's constants, so existing behaviour is unchanged when unset).
4. Add `TakerStrategy` (new `strategy/taker.h`): on each tick, if the signal is
   strong enough, emit a marketable order crossing the current best quote. Same
   `on_tick(tick, sig, outbound_actions)` shape as the maker.
5. Introduce a tiny `IStrategy` interface (just `on_tick`) with the two impls;
   `main.cpp` picks by `STRATEGY` env.

**Part C — config + launcher.**
6. Read the per-firm config from env (`FIRM_ID`, `STRATEGY`, `TOKEN_BASE`, strategy
   params, `TARGET_INSTRUMENTS`). Optionally a small per-firm `.env` file each
   process sources.
7. Add `scripts/multi_firm_run.sh` (or extend `multi_firm_demo.sh`) to launch several
   firms with distinct envs, e.g. one maker + one taker + a seed, then watch via
   `decode_audit.py` / the monitoring TUI.

## Seeding gotcha (same as before)

Firms act on **market-data ticks**; an empty book emits no ticks, so pure makers
sit idle at start and a taker has nothing to lift. A realistic run needs a **seed**
— one aggressive order or the `liquidity` tool — to get the book moving, then the
maker quotes and the taker hits. Document this in the launcher.

## Coherence with the other two plans

Implementation order across all three docs:

1. **Multi-firm identity (Part A)** — foundational; nothing else is meaningful with
   colliding tokens / a shared firm id.
2. **Private-ack channel** (`docs/private-ack-plan.md`) — per-connection, so it works
   for N firms unchanged; the **Taker** strategy especially wants acks to react to
   its own fills.
3. **Strategy diversity (Part B) + firm monitoring** (`docs/firm-monitoring-plan.md`)
   — with the caveat that firm monitoring must become **per-firm**: key the stats
   region by `FIRM_ID` (`/dev/shm/firm_stats_<FIRM_ID>`, from `FIRM_STATS_SHM_PATH`),
   so N firms each publish their own region and the Python panel can show all of
   them. Position/PnL stays ack-driven (no double-count), per that plan.

So `FIRM_ID` introduced here is the join key that makes per-firm monitoring possible.

## Testing

- Launch **two distinct firms** (a maker + a taker) via the launcher, plus a seed.
- Confirm in the audit log: **distinct firm ids**, **non-overlapping token ranges**,
  and that they **actually cross** each other (maker posts, taker lifts → fills).
- Watch live via `decode_audit.py` + the monitoring TUI (and, once firm monitoring
  lands, per-firm position/PnL panels).

## Scope & risk

~2.5–4 hrs: identity (`FIRM_ID` + `TOKEN_BASE` partition) ~45m (small but
correctness-critical — get the token arithmetic under `MAX_CLIENT_ORDERS`);
parameterise the maker ~45m; `TakerStrategy` + `IStrategy` switch ~1–2h; config +
launcher ~45m. Main risks: the token-space partition arithmetic, and making the
taker actually cross (needs a maker's quotes + a seed to have something to hit).
