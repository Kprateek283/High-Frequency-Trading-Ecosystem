"""Tier 1 — read /dev/shm/hft_stats (I4) under the seqlock, return a snapshot.

The engine's main loop is the single seqlock writer (odd seq = mid-update). We
retry until we read a stable even seq bracketing an unchanged snapshot, so the
reader never sees a torn sample. Read-only mmap; the engine is unaffected.
"""
import mmap
import os
import struct
from .. import wire


class StatsSnapshot:
    __slots__ = ("magic", "protocol_version", "num_shards", "heartbeat_tsc",
                 "dropped_reports", "dropped_drop_copies", "gateway_cycles",
                 "anchor", "shards")

    def __init__(self, buf):
        self.magic, self.protocol_version, self.num_shards = struct.unpack_from(
            "<III", buf, wire.OFF_MAGIC)
        self.heartbeat_tsc = struct.unpack_from("<Q", buf, wire.OFF_HEARTBEAT_TSC)[0]
        self.dropped_reports = struct.unpack_from("<Q", buf, wire.OFF_DROPPED_REPORTS)[0]
        self.dropped_drop_copies = struct.unpack_from("<Q", buf, wire.OFF_DROPPED_DROP_COPIES)[0]
        # epoll, read, decode, validation, enqueue, orders_processed
        c = struct.unpack_from("<QQQQQQ", buf, wire.OFF_EPOLL_CYCLES)
        self.gateway_cycles = dict(zip(
            ("epoll", "read", "decode", "validation", "enqueue", "orders_processed"), c))
        self.anchor = struct.unpack_from(wire.ANCHOR_FMT, buf, wire.OFF_ANCHOR)
        self.shards = [wire.decode_shard(buf, i) for i in range(self.num_shards)]

    def valid(self):
        return self.magic == wire.STATS_MAGIC and self.protocol_version == wire.PROTOCOL_VERSION


class StatsReader:
    def __init__(self, path):
        self.path = path
        fd = os.open(path, os.O_RDONLY)
        try:
            self.mm = mmap.mmap(fd, wire.STATS_REGION_SIZE, mmap.MAP_SHARED, mmap.PROT_READ)
        finally:
            os.close(fd)   # the mapping keeps the region alive

    def _seq(self):
        return struct.unpack_from("<I", self.mm, wire.OFF_SEQ)[0]

    def read(self, retries=100):
        """One consistent snapshot, or None if the writer never settled."""
        for _ in range(retries):
            s0 = self._seq()
            if s0 & 1:
                continue                       # writer mid-update
            snap = StatsSnapshot(self.mm)
            if self._seq() == s0:              # unchanged across the read → consistent
                return snap
        return None

    def close(self):
        self.mm.close()


def _selftest():
    import tempfile
    # Build a fixture region with wire.py itself, then read it back.
    buf = bytearray(wire.STATS_REGION_SIZE)
    struct.pack_into("<III", buf, wire.OFF_MAGIC,
                     wire.STATS_MAGIC, wire.PROTOCOL_VERSION, 4)
    struct.pack_into("<I", buf, wire.OFF_SEQ, 8)        # even = consistent
    struct.pack_into(wire.ANCHOR_FMT, buf, wire.OFF_ANCHOR, 1000, 1_000_000, 2.0)
    struct.pack_into("<Q", buf, wire.OFF_HEARTBEAT_TSC, 55555)
    struct.pack_into("<Q", buf, wire.OFF_DROPPED_DROP_COPIES, 3)
    struct.pack_into(wire.SHARD_FMT, buf, wire.SHARD_STATS_OFF, 10, 20, 0, 1, 5, 0, 0, 42)
    fd, name = tempfile.mkstemp()
    os.write(fd, buf)
    os.close(fd)
    r = StatsReader(name)
    snap = r.read()
    assert snap is not None and snap.valid()
    assert snap.num_shards == 4 and snap.heartbeat_tsc == 55555
    assert snap.dropped_drop_copies == 3
    assert snap.shards[0].orders_in == 10 and snap.shards[0].fills == 20
    assert snap.shards[0].pool_high_water == 42
    assert snap.gateway_cycles["orders_processed"] == 0
    # odd seq → read() must fail to settle
    r.mm.close()
    buf[wire.OFF_SEQ] = 7
    fd, name2 = tempfile.mkstemp()
    os.write(fd, buf)
    os.close(fd)
    r2 = StatsReader(name2)
    assert r2.read(retries=5) is None
    r2.close()
    os.unlink(name)
    os.unlink(name2)
    print("stats_reader: OK")


if __name__ == "__main__":
    _selftest()
