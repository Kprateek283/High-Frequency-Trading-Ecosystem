"""Tier 3 — rates and deltas from successive stats snapshots (I4).

Pure functions over snapshot objects (duck-typed: `.shards`, `.dropped_reports`,
`.dropped_drop_copies`, `.heartbeat_tsc`). No I/O — trivially testable.
"""
# Mirrors POOL_CAPACITY_PER_SHARD in the engine (memory_pool sizing in exchange.cpp).
# ponytail: hardcoded like the C++ side; lift to config if the shard pool ever tunes.
POOL_CAPACITY_PER_SHARD = 500000

# Every match emits exactly two drop copies -- one for the aggressor, one for the
# resting order (matching/orderbook.h, both match paths). So the `fills` counter
# counts execution REPORTS, and the number of trades is half of it.
REPORTS_PER_TRADE = 2


def _per_s(cur, prev, dt):
    return (cur - prev) / dt if dt > 0 else 0.0


def shard_rates(prev_shard, cur_shard, dt):
    fills_per_s = _per_s(cur_shard.fills, prev_shard.fills, dt)
    return {
        "orders_per_s": _per_s(cur_shard.orders_in, prev_shard.orders_in, dt),
        "fills_per_s": fills_per_s,
        "trades_per_s": fills_per_s / REPORTS_PER_TRADE,
        "cancels_per_s": _per_s(cur_shard.cancels, prev_shard.cancels, dt),
        "rejects_per_s": _per_s(cur_shard.rejects, prev_shard.rejects, dt),
    }


def trades(shard):
    """Matches this shard has executed.

    There is deliberately no "fill ratio" here. The obvious fills/orders_in is
    not a ratio of like things and cannot be made into one from these counters:

      - `orders_in` counts NEW reports, and an order that crosses immediately
        never rests, so it never emits NEW. It counts orders that RESTED.
      - `fills` counts execution reports, two per match, plus one per partial
        fill along the way.

    Run `liquidity` and the engine records 10,000 NEW against 20,000 fills --
    the old fills/orders_in displayed that as "fill%: 200". Clamping it to 100%
    only hid the symptom: the number still had no definition, and a plausible
    wrong number is worse than an obviously broken one.

    Trades are exactly derivable, so that is what we report instead.
    """
    return shard.fills / REPORTS_PER_TRADE


def pool_headroom(shard, capacity=POOL_CAPACITY_PER_SHARD):
    """Fraction of the shard pool still free (1.0 = empty, 0.0 = exhausted)."""
    return 1.0 - shard.pool_high_water / capacity if capacity else 0.0


def heartbeat_age_ns(snap, clock, now_tsc):
    """How stale the OrderManager heartbeat is, in ns (needs the §3 anchor)."""
    return clock.age_ns(snap.heartbeat_tsc, now_tsc)


def summarize(prev, cur, dt):
    """One window: per-shard rates + trade counts + headroom, plus totals and drop deltas."""
    per_shard = []
    for i, (ps, cs) in enumerate(zip(prev.shards, cur.shards)):
        row = shard_rates(ps, cs, dt)
        row.update(shard=i, trades=trades(cs), pool_headroom=pool_headroom(cs),
                   engine_q_depth=cs.engine_q_depth, dropcopy_q_depth=cs.dropcopy_q_depth,
                   mktdata_q_depth=cs.mktdata_q_depth)
        per_shard.append(row)
    return {
        "shards": per_shard,
        "orders_per_s": sum(r["orders_per_s"] for r in per_shard),
        "fills_per_s": sum(r["fills_per_s"] for r in per_shard),
        "trades_per_s": sum(r["trades_per_s"] for r in per_shard),
        "dropped_reports": cur.dropped_reports,
        "dropped_drop_copies": cur.dropped_drop_copies,
        "dropped_reports_delta": cur.dropped_reports - prev.dropped_reports,
        "dropped_drop_copies_delta": cur.dropped_drop_copies - prev.dropped_drop_copies,
    }


def _selftest():
    from types import SimpleNamespace as NS
    from .. import clock as clockmod

    def shard(orders, fills, cancels=0, rejects=0, hw=0, eq=0):
        return NS(orders_in=orders, fills=fills, cancels=cancels, rejects=rejects,
                  pool_high_water=hw, engine_q_depth=eq, dropcopy_q_depth=0, mktdata_q_depth=0)

    prev = NS(shards=[shard(100, 40)], dropped_reports=0, dropped_drop_copies=1, heartbeat_tsc=1000)
    cur = NS(shards=[shard(160, 80, hw=250000, eq=3)], dropped_reports=2, dropped_drop_copies=1, heartbeat_tsc=3000)
    s = summarize(prev, cur, dt=2.0)
    assert s["orders_per_s"] == 30.0 and s["fills_per_s"] == 20.0    # (160-100)/2, (80-40)/2
    assert s["trades_per_s"] == 10.0                                 # two reports per match
    assert abs(s["shards"][0]["pool_headroom"] - 0.5) < 1e-9         # 250000/500000
    assert s["dropped_reports_delta"] == 2 and s["dropped_drop_copies_delta"] == 0

    # The liquidity workload: 10,000 orders rest, 10,000 cross them. The engine
    # records 10,000 NEW and 20,000 execution reports. That is 10,000 trades --
    # the case the old fills/orders_in rendered as "fill%: 200".
    assert trades(shard(10000, 20000)) == 10000
    assert trades(shard(0, 0)) == 0

    c = clockmod.Clock(tsc_at_anchor=0, unix_ns_at_anchor=0, cycles_per_ns=2.0)
    assert heartbeat_age_ns(cur, c, now_tsc=5000) == (5000 - 3000) / 2.0
    print("metrics: OK")


if __name__ == "__main__":
    _selftest()
