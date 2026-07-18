"""Tier 1 — TSC↔epoch conversion from the §3 anchor (I5).

The anchor lives inside the stats region (same mmap as stats_reader); this
module only needs the three scalar values, so it reads them itself from a region
buffer rather than importing stats_reader (both are tier 1). The conversion is
pure arithmetic — no clock syscall per message — assuming an invariant TSC.
"""
import struct
from . import wire


class Clock:
    def __init__(self, tsc_at_anchor, unix_ns_at_anchor, cycles_per_ns):
        self.tsc_at_anchor = tsc_at_anchor
        self.unix_ns_at_anchor = unix_ns_at_anchor
        self.cycles_per_ns = cycles_per_ns

    def to_epoch_ns(self, tsc):
        """Wire TSC → Unix epoch nanoseconds. Handles tsc before/after the anchor."""
        delta_cycles = tsc - self.tsc_at_anchor
        return self.unix_ns_at_anchor + delta_cycles / self.cycles_per_ns

    def age_ns(self, tsc, now_tsc):
        """Cycle delta (now_tsc - tsc) rendered as nanoseconds — for heartbeat age."""
        return (now_tsc - tsc) / self.cycles_per_ns

    @classmethod
    def from_region(cls, buf):
        tsc_a, unix_a, cpn = struct.unpack_from(wire.ANCHOR_FMT, buf, wire.OFF_ANCHOR)
        return cls(tsc_a, unix_a, cpn)


def _selftest():
    # synthetic anchor: 2.0 cycles/ns, anchor at tsc=1000 → 1_000_000 ns epoch
    c = Clock(tsc_at_anchor=1000, unix_ns_at_anchor=1_000_000, cycles_per_ns=2.0)
    assert c.to_epoch_ns(1000) == 1_000_000            # at the anchor
    assert c.to_epoch_ns(3000) == 1_001_000            # +2000 cycles = +1000 ns
    assert c.to_epoch_ns(0) == 1_000_000 - 500         # before the anchor
    assert c.age_ns(1000, 3000) == 1000.0
    # from_region round-trips through the real anchor offset
    buf = bytearray(wire.STATS_REGION_SIZE)
    struct.pack_into(wire.ANCHOR_FMT, buf, wire.OFF_ANCHOR, 1000, 1_000_000, 2.0)
    c2 = Clock.from_region(buf)
    assert c2.to_epoch_ns(3000) == 1_001_000
    print("clock: OK")


if __name__ == "__main__":
    _selftest()
