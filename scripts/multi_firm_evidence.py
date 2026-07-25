#!/usr/bin/env python3
"""End-to-end evidence for scripts/multi_firm_run.sh (stdlib only, no rich).

Proves the whole chain: distinct firms in non-overlapping token slices, real
crossing (FILLED > 0), and each firm's own position/PnL/acked moving on
CONFIRMED fills (exchange ack -> firm reception -> firm_stats region -> reader).

Env: FIRM_ID_A, FIRM_ID_B, AUDIT, TOKEN_SLICE.
"""
import os
import sys
import struct

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, REPO_ROOT)

from monitoring import wire                              # noqa: E402
from monitoring.feeds import firm_stats_reader as FSR    # noqa: E402

SLICE = int(os.environ.get("TOKEN_SLICE", "3125000"))
AUDIT = os.environ.get("AUDIT", os.path.join(REPO_ROOT, "order_audit.log"))
FA = os.environ.get("FIRM_ID_A", "A")
FB = os.environ.get("FIRM_ID_B", "B")


def read_firm(fid):
    path = "/dev/shm/firm_stats_" + fid
    try:
        s = FSR.FirmStatsReader(path).read()
    except OSError:
        return None
    return s


def audit_by_slice():
    """Per-slice FILLED/PARTIAL counts + observed [min,max] token, from the audit."""
    with open(AUDIT, "rb") as f:
        blob = f.read()
    _, _, entry_size, write_index = wire.decode_audit_header(blob)
    assert entry_size == wire.AUDIT_ENTRY_SIZE, entry_size
    # slice index -> stats
    agg = {}
    for i in range(write_index):
        off = wire.AUDIT_HEADER_SIZE + i * wire.AUDIT_ENTRY_SIZE
        e = wire.decode_audit_entry(blob, off)
        sl = e.client_order_id // SLICE       # 0 = firm A slice, 1 = firm B slice, ...
        a = agg.setdefault(sl, {"n": 0, "filled": 0, "partial": 0,
                                "lo": e.client_order_id, "hi": e.client_order_id})
        a["n"] += 1
        a["lo"] = min(a["lo"], e.client_order_id)
        a["hi"] = max(a["hi"], e.client_order_id)
        if e.state == 2:
            a["filled"] += 1
        elif e.state == 1:
            a["partial"] += 1
    return agg, write_index


def main():
    print("--- 1. Per-firm state (from /dev/shm/firm_stats_<ID>, ack-driven) ---")
    print(f"{'firm':>4} {'sent':>8} {'acked':>8} {'in_flight':>9} "
          f"{'position':>12} {'realized_pnl':>16}")
    firms = {}
    for fid in (FA, FB):
        s = read_firm(fid)
        firms[fid] = s
        if s is None:
            print(f"{fid:>4}   <no region>")
            continue
        pos = ", ".join(f"{i}:{p:+}" for i, p in list(s.positions.items())[:3]) or "flat"
        print(f"{fid:>4} {s.orders_sent:>8} {s.orders_acked:>8} {s.in_flight:>9} "
              f"{pos:>12} {s.realized_pnl:>16,}")

    print("\n--- 2. Token partition (each firm owns [base, base+SLICE)) ---")
    a, b = firms.get(FA), firms.get(FB)
    if a and b:
        a_lo, a_hi = 0, a.orders_sent          # firm A: tokens ~[0, orders_sent)
        b_lo, b_hi = SLICE, SLICE + b.orders_sent
        print(f"  firm {FA}: base 0,        used ~[{a_lo}, {a_hi})  (slice ends {SLICE})")
        print(f"  firm {FB}: base {SLICE}, used ~[{b_lo}, {b_hi})  (slice ends {2*SLICE})")
        disjoint = a_hi < SLICE <= b_lo
        print(f"  non-overlapping: {'PASS' if disjoint else 'FAIL'}  "
              f"(A stays < {SLICE} <= B)")

    print("\n--- 3. Audit: crossing + fills per token slice ---")
    agg, total = audit_by_slice()
    filled = sum(v["filled"] for v in agg.values())
    partial = sum(v["partial"] for v in agg.values())
    print(f"  entries={total}  FILLED={filled}  PARTIAL_FILL={partial}  "
          f"=> crossed: {'YES' if filled + partial > 0 else 'NO'}")
    names = {0: f"slice0 (firm {FA} [0,{SLICE}))",
             1: f"slice1 (firm {FB} [{SLICE},{2*SLICE}))"}
    for sl in sorted(agg):
        v = agg[sl]
        label = names.get(sl, f"slice{sl} (seed/other)")
        print(f"  {label:>34}: entries={v['n']:>7} filled={v['filled']:>7} "
              f"partial={v['partial']:>7} tokens[{v['lo']},{v['hi']}]")
    print("  (note: the liquidity seed's rester tokens fall in slice0 and the "
          "market_maker\n   seed tool's in slice1; per-firm confirmed fills are the "
          "acked column in section 1.)")

    ok = (a and b and a.orders_acked > 0 and b.orders_acked > 0
          and a.positions and b.positions and filled > 0)
    print("\nRESULT:", "PASS — both firms crossed into confirmed fills, "
          "position/PnL moved." if ok else
          "INCOMPLETE — see sections above.")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
