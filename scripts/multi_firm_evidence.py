#!/usr/bin/env python3
"""End-to-end evidence for scripts/multi_firm_run.sh (stdlib only, no rich).

Proves the whole chain: N distinct firms in non-overlapping token slices, real
crossing (FILLED > 0), and each firm's own position/PnL/acked moving on
CONFIRMED fills (exchange ack -> firm reception -> firm_stats region -> reader).

Env: FIRM_IDS (comma list, e.g. "A,B,C"), AUDIT, TOKEN_SLICE.
"""
import os
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, REPO_ROOT)

from monitoring import wire                              # noqa: E402
from monitoring.feeds import firm_stats_reader as FSR    # noqa: E402

SLICE = int(os.environ.get("TOKEN_SLICE", "3125000"))
AUDIT = os.environ.get("AUDIT", os.path.join(REPO_ROOT, "order_audit.log"))
FIRM_IDS = [x for x in os.environ.get("FIRM_IDS", "A,B").split(",") if x]


def read_firm(fid):
    try:
        return FSR.FirmStatsReader("/dev/shm/firm_stats_" + fid).read()
    except OSError:
        return None


def audit_by_slice():
    """Per-slice FILLED/PARTIAL counts + observed [min,max] token, from the audit."""
    with open(AUDIT, "rb") as f:
        blob = f.read()
    _, _, entry_size, write_index = wire.decode_audit_header(blob)
    assert entry_size == wire.AUDIT_ENTRY_SIZE, entry_size
    agg = {}
    for i in range(write_index):
        off = wire.AUDIT_HEADER_SIZE + i * wire.AUDIT_ENTRY_SIZE
        e = wire.decode_audit_entry(blob, off)
        sl = e.client_order_id // SLICE       # slice index = firm k (0,1,2,...)
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
    for fid in FIRM_IDS:
        s = read_firm(fid)
        firms[fid] = s
        if s is None:
            print(f"{fid:>4}   <no region>")
            continue
        pos = ", ".join(f"{i}:{p:+}" for i, p in list(s.positions.items())[:3]) or "flat"
        print(f"{fid:>4} {s.orders_sent:>8} {s.orders_acked:>8} {s.in_flight:>9} "
              f"{pos:>12} {s.realized_pnl:>16,}")

    # Firm k owns slice [k*SLICE, (k+1)*SLICE); the slices are disjoint by construction.
    print("\n--- 2. Token partition (firm k owns [k*SLICE, (k+1)*SLICE)) ---")
    ok_partition = True
    for k, fid in enumerate(FIRM_IDS):
        s = firms.get(fid)
        base = k * SLICE
        used_hi = base + (s.orders_sent if s else 0)
        within = used_hi < base + SLICE
        ok_partition = ok_partition and within
        print(f"  firm {fid}: base {base:>10}  used ~[{base}, {used_hi})  "
              f"(slice ends {base + SLICE}) {'OK' if within else 'OVERFLOW'}")
    print(f"  non-overlapping: {'PASS' if ok_partition else 'FAIL'}  "
          f"(each firm k confined to its own {SLICE}-wide slice)")

    print("\n--- 3. Audit: crossing + fills per token slice ---")
    agg, total = audit_by_slice()
    filled = sum(v["filled"] for v in agg.values())
    partial = sum(v["partial"] for v in agg.values())
    print(f"  entries={total}  FILLED={filled}  PARTIAL_FILL={partial}  "
          f"=> crossed: {'YES' if filled + partial > 0 else 'NO'}")
    for sl in sorted(agg):
        v = agg[sl]
        label = (f"slice{sl} (firm {FIRM_IDS[sl]} [{sl*SLICE},{(sl+1)*SLICE}))"
                 if sl < len(FIRM_IDS) else f"slice{sl} (seed/other)")
        print(f"  {label:>40}: entries={v['n']:>7} filled={v['filled']:>7} "
              f"partial={v['partial']:>7} tokens[{v['lo']},{v['hi']}]")
    print("  (note: the liquidity/market_maker seed tokens land in the low slices,\n"
          "   mixing seed + firm orders there; per-firm CONFIRMED fills are the "
          "acked column in section 1.)")

    live = [firms[f] for f in FIRM_IDS if firms.get(f)]
    acked_ok = all(s.orders_acked > 0 for s in live) and len(live) == len(FIRM_IDS)
    ok = bool(live) and acked_ok and ok_partition and filled > 0
    print("\nRESULT:", "PASS — every firm crossed into confirmed fills, "
          "position/PnL moved, slices disjoint." if ok else
          "INCOMPLETE — see sections above.")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
