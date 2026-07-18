"""Tier 0 — domain types (plain dataclasses, no I/O, no imports).

wire.py returns flat wire-records (namedtuples). Tier-2+ modules build these
richer domain objects from those records: `orderbook` builds BookLevels,
`portfolio` builds Positions. Kept separate so tier 0 has no internal edges.
The enums mirror protocol/messages.h / matching/order.h by value.
"""
from dataclasses import dataclass, field
from enum import IntEnum


class Side(IntEnum):
    BUY = 0
    SELL = 1


class OrderState(IntEnum):
    NEW = 0
    PARTIAL_FILL = 1
    FILLED = 2
    CANCELED = 3
    REJECTED = 4


@dataclass
class Order:
    internal_id: int
    instrument_id: int
    side: Side
    price: int
    quantity: int


@dataclass
class Fill:
    instrument_id: int
    side: Side
    price: int
    quantity: int
    timestamp_tsc: int = 0


@dataclass
class BookLevel:
    price: int
    quantity: int = 0
    orders: int = 0


@dataclass
class Position:
    instrument_id: int
    net_qty: int = 0
    realized_cash: int = 0   # signed: +received on sells, -paid on buys


def _selftest():
    assert Side.SELL == 1 and OrderState.FILLED == 2
    lvl = BookLevel(price=50000)
    lvl.quantity += 100
    assert lvl.quantity == 100 and lvl.orders == 0
    print("models: OK")


if __name__ == "__main__":
    _selftest()
