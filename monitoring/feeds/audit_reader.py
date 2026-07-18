"""Tier 1 — read order_audit.log (I2): validate header, tail by write_index.

The engine publishes write_index (release) after each committed 64-byte entry,
so the same code tails a live log and replays a post-crash one: bound every scan
by the current write_index, re-read it to pick up new entries. mmap keeps it
zero-syscall per poll.
"""
import mmap
import os
import struct
from .. import wire


class AuditReader:
    def __init__(self, path):
        self.path = path
        self.fd = os.open(path, os.O_RDONLY)
        size = os.fstat(self.fd).st_size
        self.mm = mmap.mmap(self.fd, size, mmap.MAP_SHARED, mmap.PROT_READ)
        magic, version, entry_size, _ = wire.decode_audit_header(self.mm)
        if magic != wire.AUDIT_MAGIC:
            raise ValueError(f"bad audit magic {magic:#x}")
        if version != wire.PROTOCOL_VERSION:
            raise ValueError(f"audit version {version} != {wire.PROTOCOL_VERSION}")
        if entry_size != wire.AUDIT_ENTRY_SIZE:
            raise ValueError(f"audit entry_size {entry_size} != {wire.AUDIT_ENTRY_SIZE}")
        self.cursor = 0        # entries already yielded

    def write_index(self):
        # acquire-load semantics: on x86 a plain aligned load suffices
        return struct.unpack_from("<Q", self.mm, 16)[0]

    def poll(self):
        """Yield entries committed since the last poll (live tail or replay)."""
        end = self.write_index()
        while self.cursor < end:
            off = wire.AUDIT_HEADER_SIZE + self.cursor * wire.AUDIT_ENTRY_SIZE
            yield wire.decode_audit_entry(self.mm, off)
            self.cursor += 1

    def close(self):
        self.mm.close()
        os.close(self.fd)


def _selftest():
    import tempfile
    from ..models import OrderState, Side
    # Write a fixture log with wire.py: header + 2 entries, write_index=2.
    n = 2
    buf = bytearray(wire.AUDIT_HEADER_SIZE + n * wire.AUDIT_ENTRY_SIZE)
    struct.pack_into(wire.AUDIT_HEADER_FMT, buf, 0,
                     wire.AUDIT_MAGIC, wire.PROTOCOL_VERSION, wire.AUDIT_ENTRY_SIZE, 0)
    for i in range(n):
        off = wire.AUDIT_HEADER_SIZE + i * wire.AUDIT_ENTRY_SIZE
        struct.pack_into("<Q", buf, off, 1000 + i)              # timestamp_tsc
        struct.pack_into(wire.DROPCOPY_FMT, buf, off + wire.DROPCOPY_OFF,
                         i, i, 50000, 100, 7, int(OrderState.FILLED), int(Side.BUY))
    struct.pack_into("<Q", buf, 16, 1)                          # publish only 1 entry first
    fd, name = tempfile.mkstemp()
    os.write(fd, buf)
    os.close(fd)

    r = AuditReader(name)
    first = list(r.poll())
    assert len(first) == 1 and first[0].client_order_id == 0        # only committed one
    # engine commits the second entry → bump write_index in the file; the read-only
    # MAP_SHARED mapping sees the store, exactly as it sees the live engine's.
    wfd = os.open(name, os.O_WRONLY)
    os.pwrite(wfd, struct.pack("<Q", 2), 16)
    os.close(wfd)
    second = list(r.poll())
    assert len(second) == 1 and second[0].client_order_id == 1      # tail picked it up
    assert wire.ORDER_STATE[second[0].state] == "FILLED"
    assert list(r.poll()) == []                                    # nothing new
    r.close()
    os.unlink(name)
    print("audit_reader: OK")


if __name__ == "__main__":
    _selftest()
