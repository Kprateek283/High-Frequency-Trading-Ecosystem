#!/usr/bin/env python3
"""Decode order_audit.log into per-state counts. Stdlib only.

The log is a 64-byte AuditLogHeader (magic, version, entry_size, write_index)
followed by write_index OrderLogEntry records of 64 bytes each. Each entry is a
uint64 timestamp then a 32-byte DropCopyMessage. DropCopyMessage is alignas(32),
so it sits at offset 32 in the entry (not 8) -- the timestamp is followed by 24
bytes of padding. The state byte is 30 into the message, i.e. offset 62. Layout
is pinned by PROTOCOL_VERSION, so these offsets are fixed.
"""
import struct
import sys

HEADER = 64
ENTRY = 64
STATE_OFF = 62  # msg at 32 (alignas(32)) + DropCopyMessage.state offset(30)
STATES = {0: "NEW", 1: "PARTIAL_FILL", 2: "FILLED", 3: "CANCELED", 4: "REJECTED"}


def decode(path):
    with open(path, "rb") as f:
        blob = f.read()
    if len(blob) < HEADER:
        raise SystemExit(f"{path}: shorter than a header")
    # The header records the writer's sizeof(OrderLogEntry) at offset 12; if it
    # disagrees with our stride the struct layout drifted and every offset below
    # is wrong. Fail loudly instead of silently miscounting (the bug this file
    # started life with).
    entry_size = struct.unpack_from("<I", blob, 12)[0]
    if entry_size != ENTRY:
        raise SystemExit(f"{path}: entry_size {entry_size} != {ENTRY}; layout drifted")
    # write_index is the committed entry count, at offset 16 in the header.
    write_index = struct.unpack_from("<Q", blob, 16)[0]
    counts = {name: 0 for name in STATES.values()}
    for i in range(write_index):
        off = HEADER + i * ENTRY + STATE_OFF
        counts[STATES.get(blob[off], f"?{blob[off]}")] = \
            counts.get(STATES.get(blob[off], f"?{blob[off]}"), 0) + 1
    counts["entries"] = write_index
    counts["matches"] = counts["FILLED"] + counts["PARTIAL_FILL"]
    return counts


if __name__ == "__main__":
    path = sys.argv[1] if len(sys.argv) > 1 else "order_audit.log"
    c = decode(path)
    for k in ("entries", "NEW", "matches", "FILLED", "PARTIAL_FILL",
              "CANCELED", "REJECTED"):
        print(f"{k:14}: {c[k]}")
