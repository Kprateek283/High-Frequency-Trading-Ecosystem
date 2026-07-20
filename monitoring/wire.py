"""Tier 0 — the fourth copy of the wire schema, kept honest by calcsize.

Field-for-field mirror of hft_engine/src/protocol/messages.h and the two mmap'd
layouts (order_audit.log header + OrderLogEntry, and /dev/shm HftStatsRegion).
Every `struct.calcsize` here is asserted equal to the C++ `static_assert` size,
so a layout drift on either side fails a test instead of silently mis-decoding.
`<` = little-endian, no padding — matches `#pragma pack(1)` on the target x86-64.
"""
import struct
from collections import namedtuple

PROTOCOL_VERSION = 1
MAX_INSTRUMENTS = 256
INVALID_INSTRUMENT = 0xFFFF

# --- enums (mirror protocol/messages.h + matching/order.h) ---
SIDE = {0: "BUY", 1: "SELL"}          # Side enum; ITCH/OUCH carry it as ASCII 'B'/'S'
ORDER_STATE = {0: "NEW", 1: "PARTIAL_FILL", 2: "FILLED", 3: "CANCELED", 4: "REJECTED"}

# --- ITCH 5.0 outbound market data (I1) ---
ITCH_FMT = "<cHHQQIQc"          # msg_type, stock_locate, tracking_number, timestamp,
ITCH_SIZE = struct.calcsize(ITCH_FMT)   # internal_id, shares, price, side
ItchMessage = namedtuple(
    "ItchMessage",
    "msg_type stock_locate tracking_number timestamp internal_id shares price side")

def decode_itch(buf, off=0):
    msg_type, sl, tn, ts, iid, sh, px, side = struct.unpack_from(ITCH_FMT, buf, off)
    return ItchMessage(msg_type.decode(), sl, tn, ts, iid, sh, px, side.decode())

def iter_itch(buf):
    """Decode a datagram carrying 1..N back-to-back ItchMessages."""
    for off in range(0, len(buf) - ITCH_SIZE + 1, ITCH_SIZE):
        yield decode_itch(buf, off)

# --- OUCH 4.2 inbound order (I3 / I6 orders.bin) ---
OUCH_ENTER_FMT = "<c14scI8sII4scccIccQQQQ"
OUCH_ENTER_SIZE = struct.calcsize(OUCH_ENTER_FMT)

# --- Audit log (I2): 64B header + 64B OrderLogEntry[] ---
AUDIT_MAGIC = 0x48465441554401       # "HFTAUD" + version byte
AUDIT_HEADER_FMT = "<QIIQ"           # magic, version, entry_size, write_index
AUDIT_HEADER_SIZE = 64               # struct is alignas(64); trailing pad after write_index
AUDIT_ENTRY_SIZE = 64                # {uint64 timestamp_tsc; DropCopyMessage(alignas32) msg;}
# DropCopyMessage sits at offset 32 inside the entry (alignas(32) → 24B pad after ts).
DROPCOPY_OFF = 32
DROPCOPY_FMT = "<QQQIHBB"            # client_order_id, internal_id, price, quantity,
DROPCOPY_SIZE = struct.calcsize(DROPCOPY_FMT)   # instrument_id, state, side
DropCopy = namedtuple(
    "DropCopy", "timestamp_tsc client_order_id internal_id price quantity "
                "instrument_id state side")

def decode_audit_header(buf, off=0):
    magic, version, entry_size, write_index = struct.unpack_from(AUDIT_HEADER_FMT, buf, off)
    return magic, version, entry_size, write_index

def decode_audit_entry(buf, off=0):
    (ts,) = struct.unpack_from("<Q", buf, off)
    coid, iid, px, qty, inst, state, side = struct.unpack_from(DROPCOPY_FMT, buf, off + DROPCOPY_OFF)
    return DropCopy(ts, coid, iid, px, qty, inst, state, side)

# --- Stats region (I4/I5): HftStatsRegion in /dev/shm ---
STATS_MAGIC = 0x48465431              # "HFT1"
STATS_MAX_SHARDS = 8
STATS_REGION_SIZE = 640
SHARD_STATS_OFF = 128
SHARD_STATS_SIZE = 64
# scalar field offsets (see stats_region.h; confirmed by C++ offsetof)
OFF_MAGIC = 0
OFF_PROTOCOL_VERSION = 4
OFF_NUM_SHARDS = 8
OFF_SEQ = 16
OFF_ANCHOR = 24            # tsc_at_anchor(Q), unix_ns_at_anchor(Q), cycles_per_ns(d)
OFF_HEARTBEAT_TSC = 48
OFF_DROPPED_REPORTS = 56
OFF_DROPPED_DROP_COPIES = 64
OFF_EPOLL_CYCLES = 72     # epoll,read,decode,validation,enqueue,orders_processed (6×Q)
ANCHOR_FMT = "<QQd"
SHARD_FMT = "<QQQQQQQQ"    # orders_in fills cancels rejects engine_q dropcopy_q mktdata_q pool_hw
ShardStats = namedtuple(
    "ShardStats", "orders_in fills cancels rejects engine_q_depth "
                  "dropcopy_q_depth mktdata_q_depth pool_high_water")

def decode_shard(buf, i):
    off = SHARD_STATS_OFF + i * SHARD_STATS_SIZE
    return ShardStats(*struct.unpack_from(SHARD_FMT, buf, off))


def _selftest():
    # The whole point of this module: sizes equal the C++ static_asserts.
    assert ITCH_SIZE == 34, ITCH_SIZE
    assert OUCH_ENTER_SIZE == 81, OUCH_ENTER_SIZE
    assert DROPCOPY_SIZE == 32, DROPCOPY_SIZE
    assert AUDIT_ENTRY_SIZE == 64 and AUDIT_HEADER_SIZE == 64
    assert STATS_REGION_SIZE == SHARD_STATS_OFF + STATS_MAX_SHARDS * SHARD_STATS_SIZE
    # round-trip one ITCH message
    raw = struct.pack(ITCH_FMT, b"A", 5, 7, 111, 222, 100, 50000, b"B")
    m = decode_itch(raw)
    assert m.msg_type == "A" and m.stock_locate == 5 and m.price == 50000 and m.side == "B"
    assert len(list(iter_itch(raw * 3))) == 3
    # round-trip an audit entry at its real 64-byte stride
    entry = bytearray(64)
    struct.pack_into("<Q", entry, 0, 999)                       # timestamp_tsc
    struct.pack_into(DROPCOPY_FMT, entry, DROPCOPY_OFF, 42, 7, 50000, 100, 3, 2, 1)
    d = decode_audit_entry(entry)
    assert d.timestamp_tsc == 999 and d.client_order_id == 42 and d.instrument_id == 3
    assert ORDER_STATE[d.state] == "FILLED" and SIDE[d.side] == "SELL"
    print("wire: OK")


if __name__ == "__main__":
    _selftest()
