"""Tier 1 — read /dev/shm/firm_stats_<FIRM_ID> under the seqlock, return a snapshot.

Symmetric to feeds/stats_reader.py but against the FIRM's region (the firm's
market-data callback thread is the single seqlock writer; odd seq = mid-update).
We retry until an even seq brackets an unchanged read, so the reader never sees
a torn sample. Read-only mmap; the firm is unaffected.

Layout mirrors hft-trading-firm/src/monitoring/firm_stats.h (FirmStatsRegion);
the offsets here are locked to the static_asserts in that header.
"""
import glob
import mmap
import os
import struct

FIRM_STATS_MAGIC = 0x4649524D          # "FIRM"
PROTOCOL_VERSION = 1
FIRM_MAX_INSTRUMENTS = 256

# Scalar field offsets (see firm_stats.h; locked by C++ static_assert/offsetof).
OFF_MAGIC = 0             # magic(I), protocol_version(I)
OFF_SEQ = 8               # seqlock
OFF_KILL_SWITCH = 12
OFF_HEARTBEAT_TSC = 16
OFF_SCALARS = 24          # realized_pnl(q) + 13 cumulative counters (Q)
OFF_POSITION = 136        # int64[FIRM_MAX_INSTRUMENTS]
# realized_pnl, orders_sent, orders_acked, in_flight, ticks,
# book, signal, strategy, risk, exec, serialize, send, enqueue, dequeue
SCALARS_FMT = "<q13Q"
_CYCLE_NAMES = ("book", "signal", "strategy", "risk", "exec",
                "serialize", "send", "enqueue", "dequeue")

FIRM_STATS_CONTENT_SIZE = OFF_POSITION + FIRM_MAX_INSTRUMENTS * 8   # 2184
# The C++ struct is alignas(64); the file is ftruncate'd to that padded sizeof,
# so mmap the same size the writer created (2240).
FIRM_STATS_REGION_SIZE = (FIRM_STATS_CONTENT_SIZE + 63) & ~63       # 2240


class FirmStatsSnapshot:
    __slots__ = ("magic", "protocol_version", "seq", "kill_switch", "heartbeat_tsc",
                 "realized_pnl", "orders_sent", "orders_acked", "in_flight", "ticks",
                 "cycles", "positions")

    def __init__(self, buf):
        self.magic, self.protocol_version = struct.unpack_from("<II", buf, OFF_MAGIC)
        self.seq = struct.unpack_from("<I", buf, OFF_SEQ)[0]
        self.kill_switch = struct.unpack_from("<I", buf, OFF_KILL_SWITCH)[0]
        self.heartbeat_tsc = struct.unpack_from("<Q", buf, OFF_HEARTBEAT_TSC)[0]
        v = struct.unpack_from(SCALARS_FMT, buf, OFF_SCALARS)
        (self.realized_pnl, self.orders_sent, self.orders_acked,
         self.in_flight, self.ticks) = v[:5]
        self.cycles = dict(zip(_CYCLE_NAMES, v[5:]))
        pos = struct.unpack_from("<%dq" % FIRM_MAX_INSTRUMENTS, buf, OFF_POSITION)
        self.positions = {i: p for i, p in enumerate(pos) if p != 0}   # non-flat only

    def valid(self):
        return self.magic == FIRM_STATS_MAGIC and self.protocol_version == PROTOCOL_VERSION


class FirmStatsReader:
    def __init__(self, path):
        self.path = path
        self.firm_id = firm_id_from_path(path)
        fd = os.open(path, os.O_RDONLY)
        try:
            self.mm = mmap.mmap(fd, FIRM_STATS_REGION_SIZE, mmap.MAP_SHARED, mmap.PROT_READ)
        finally:
            os.close(fd)   # the mapping keeps the region alive

    def _seq(self):
        return struct.unpack_from("<I", self.mm, OFF_SEQ)[0]

    def read(self, retries=100):
        """One consistent snapshot, or None if the writer never settled."""
        for _ in range(retries):
            s0 = self._seq()
            if s0 & 1:
                continue                       # writer mid-update
            snap = FirmStatsSnapshot(self.mm)
            if self._seq() == s0:              # unchanged across the read → consistent
                return snap
        return None

    def close(self):
        self.mm.close()


def firm_id_from_path(path):
    """/dev/shm/firm_stats_HFT1 -> 'HFT1'."""
    base = os.path.basename(path)
    prefix = "firm_stats_"
    return base[len(prefix):] if base.startswith(prefix) else base


def discover(prefix="/dev/shm/firm_stats_"):
    """Every per-firm region currently in /dev/shm, sorted by path."""
    return sorted(glob.glob(prefix + "*"))


def _selftest():
    import tempfile
    # Build a fixture region with this module's own layout, then read it back.
    buf = bytearray(FIRM_STATS_REGION_SIZE)
    struct.pack_into("<II", buf, OFF_MAGIC, FIRM_STATS_MAGIC, PROTOCOL_VERSION)
    struct.pack_into("<I", buf, OFF_SEQ, 4)             # even = consistent
    struct.pack_into("<I", buf, OFF_KILL_SWITCH, 1)
    struct.pack_into("<Q", buf, OFF_HEARTBEAT_TSC, 777)
    struct.pack_into(SCALARS_FMT, buf, OFF_SCALARS,
                     -1500,          # realized_pnl (a loss)
                     10, 7, 3, 42,   # orders_sent, acked, in_flight, ticks
                     1, 2, 3, 4, 5,  # book, signal, strategy, risk, exec
                     6, 7, 8, 9)     # serialize, send, enqueue, dequeue
    struct.pack_into("<q", buf, OFF_POSITION + 5 * 8, 250)   # instrument 5 long 250

    fd, name = tempfile.mkstemp()
    os.write(fd, buf)
    os.close(fd)
    r = FirmStatsReader(name)
    assert r.firm_id == firm_id_from_path(name)
    snap = r.read()
    assert snap is not None and snap.valid()
    assert snap.kill_switch == 1 and snap.heartbeat_tsc == 777
    assert snap.realized_pnl == -1500
    assert snap.orders_sent == 10 and snap.orders_acked == 7 and snap.in_flight == 3
    assert snap.ticks == 42
    assert snap.cycles["strategy"] == 3 and snap.cycles["dequeue"] == 9
    assert snap.positions == {5: 250}
    r.close()

    # odd seq → read() must fail to settle
    buf[OFF_SEQ] = 3
    fd, name2 = tempfile.mkstemp()
    os.write(fd, buf)
    os.close(fd)
    r2 = FirmStatsReader(name2)
    assert r2.read(retries=5) is None
    r2.close()

    assert firm_id_from_path("/dev/shm/firm_stats_TAKR") == "TAKR"
    os.unlink(name)
    os.unlink(name2)
    print("firm_stats_reader: OK")


if __name__ == "__main__":
    _selftest()
