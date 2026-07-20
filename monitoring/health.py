"""Tier 4 — turn a stats snapshot into a health verdict (I4).

Pure assessment over a snapshot + the §3 clock: heartbeat freshness, queue-depth
ceilings, pool headroom, and drop counters. Thresholds are module constants so
they tune in one place. `assess()` is what the TUI's health panel renders.
"""
from .core import metrics

# Tunable ceilings. Queues are sized 2^20; heartbeat is written every OrderManager
# loop, so anything past ~half a second means the drain has stalled.
HEARTBEAT_STALE_NS = 500_000_000        # 0.5s
# A heartbeat timestamped slightly ahead of us is ordinary jitter: the engine and
# this reader sample the clock at different instants. A large negative age is not
# jitter -- it means the TSC anchor is wrong, so every timestamp we convert is
# wrong. Report it, because the staleness test above cannot: it only fires on
# ages that are too POSITIVE, so a drifting anchor reads as healthy forever.
HEARTBEAT_SKEW_NS = 50_000_000          # 50ms
QUEUE_DEPTH_WARN = 100_000              # ~10% of a 2^20 queue
POOL_HEADROOM_WARN = 0.10               # < 10% free

OK, WARN, CRITICAL = "OK", "WARN", "CRITICAL"
_RANK = {OK: 0, WARN: 1, CRITICAL: 2}


def _worst(*levels):
    return max(levels, key=lambda l: _RANK[l])


def assess(snap, clock, now_tsc):
    reasons = []
    overall = OK

    age = metrics.heartbeat_age_ns(snap, clock, now_tsc)
    if age > HEARTBEAT_STALE_NS:
        overall = _worst(overall, CRITICAL)
        reasons.append(f"heartbeat stale ({age/1e6:.0f}ms)")
    elif age < -HEARTBEAT_SKEW_NS:
        # WARN, not CRITICAL: the engine may be perfectly healthy. What is broken
        # is our ability to judge it, so say that rather than implying an outage.
        overall = _worst(overall, WARN)
        reasons.append(f"heartbeat {abs(age)/1e6:.0f}ms in the future "
                       f"— TSC anchor skew, timing is unreliable")

    if snap.dropped_reports or snap.dropped_drop_copies:
        overall = _worst(overall, WARN)
        reasons.append(f"drops: reports={snap.dropped_reports} "
                       f"drop_copies={snap.dropped_drop_copies}")

    shard_health = []
    for i, sh in enumerate(snap.shards):
        level = OK
        why = []
        depth = max(sh.engine_q_depth, sh.dropcopy_q_depth, sh.mktdata_q_depth)
        if depth > QUEUE_DEPTH_WARN:
            level = _worst(level, WARN)
            why.append(f"queue depth {depth}")
        headroom = metrics.pool_headroom(sh)
        if headroom < POOL_HEADROOM_WARN:
            level = _worst(level, CRITICAL)
            why.append(f"pool {headroom*100:.0f}% free")
        shard_health.append({"shard": i, "level": level, "why": why,
                             "max_depth": depth, "pool_headroom": headroom})
        overall = _worst(overall, level)

    return {"overall": overall, "heartbeat_age_ns": age,
            "reasons": reasons, "shards": shard_health}


def _selftest():
    from types import SimpleNamespace as NS
    from . import clock as clockmod

    def shard(hw=0, eq=0, dq=0, mq=0):
        return NS(pool_high_water=hw, engine_q_depth=eq, dropcopy_q_depth=dq, mktdata_q_depth=mq)

    c = clockmod.Clock(tsc_at_anchor=0, unix_ns_at_anchor=0, cycles_per_ns=1.0)

    # healthy: fresh heartbeat, empty queues, empty pool, no drops
    good = NS(shards=[shard(), shard()], dropped_reports=0, dropped_drop_copies=0, heartbeat_tsc=1_000_000_000)
    r = assess(good, c, now_tsc=1_000_000_100)      # ~100ns old
    assert r["overall"] == OK and not r["reasons"]

    # stale heartbeat → CRITICAL
    r = assess(good, c, now_tsc=1_000_000_000 + 600_000_000)   # 0.6s old
    assert r["overall"] == CRITICAL and "heartbeat" in r["reasons"][0]

    # heartbeat from the future → WARN, not a silent OK (D2). A drifting TSC
    # anchor produces exactly this, and the staleness test above cannot see it.
    r = assess(good, c, now_tsc=1_000_000_000 - 200_000_000)   # 0.2s in the future
    assert r["overall"] == WARN, "future heartbeat must not read as healthy"
    assert "future" in r["reasons"][0]
    # small skew is ordinary jitter and must stay quiet
    r = assess(good, c, now_tsc=1_000_000_000 - 1_000_000)     # 1ms in the future
    assert r["overall"] == OK and not r["reasons"]

    # deep queue → WARN; near-full pool → CRITICAL; a drop → WARN reason present
    bad = NS(shards=[shard(hw=495000, eq=200_000)], dropped_reports=5, dropped_drop_copies=0,
             heartbeat_tsc=1_000_000_000)
    r = assess(bad, c, now_tsc=1_000_000_100)
    assert r["overall"] == CRITICAL                 # pool dominates
    assert r["shards"][0]["max_depth"] == 200_000
    assert any("drops" in x for x in r["reasons"])
    print("health: OK")


if __name__ == "__main__":
    _selftest()
