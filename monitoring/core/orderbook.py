"""Tier 2 — reconstruct per-instrument L2 books from the ITCH stream (I1).

Order-reference ids recycle: the engine's internal_id is a pool slot index, freed
when an order leaves the book and reused by the next `A`. So an `A` is
authoritative — it starts a fresh order for that id, overwriting any stale one.
Engine ITCH semantics (matching/orderbook.h broadcast()):
  'A' ref rests: (side, price, shares)      — only the leftover after matching
  'E' ref executes `shares` at `price`      — emitted for both resting & aggressor;
      the aggressor's ref never rested, so an 'E' for an unknown ref is ignored
  'X' ref removed
"""
from .. import wire
from ..models import BookLevel


class Book:
    """One instrument's book: resting orders + aggregated price levels."""
    def __init__(self, instrument_id):
        self.instrument_id = instrument_id
        self.orders = {}                 # ref -> (side_char, price, remaining_qty)
        self.bids = {}                   # price -> BookLevel
        self.asks = {}                   # price -> BookLevel

    def _side(self, side_char):
        return self.bids if side_char == "B" else self.asks

    def _level_add(self, side_char, price, qty):
        levels = self._side(side_char)
        lvl = levels.get(price) or BookLevel(price=price)
        lvl.quantity += qty
        lvl.orders += 1
        levels[price] = lvl

    def _level_sub(self, side_char, price, qty, drop_order):
        levels = self._side(side_char)
        lvl = levels.get(price)
        if not lvl:
            return
        lvl.quantity -= qty
        if drop_order:
            lvl.orders -= 1
        if lvl.quantity <= 0 or lvl.orders <= 0:
            del levels[price]

    def _remove_order(self, ref):
        side_char, price, remaining = self.orders.pop(ref)
        self._level_sub(side_char, price, remaining, drop_order=True)

    def apply(self, msg):
        t = msg.msg_type
        ref = msg.internal_id
        if t == "A":
            if ref in self.orders:               # recycled id still on the books: drop the stale one
                self._remove_order(ref)
            self.orders[ref] = (msg.side, msg.price, msg.shares)
            self._level_add(msg.side, msg.price, msg.shares)
        elif t == "E":
            o = self.orders.get(ref)
            if o is None:
                return                            # aggressor ref never rested — nothing to reduce
            side_char, price, remaining = o
            remaining -= msg.shares
            if remaining <= 0:
                self._remove_order(ref)
            else:
                self.orders[ref] = (side_char, price, remaining)
                self._level_sub(side_char, price, msg.shares, drop_order=False)
        elif t == "X":
            if ref in self.orders:
                self._remove_order(ref)

    def best_bid(self):
        return max(self.bids) if self.bids else None

    def best_ask(self):
        return min(self.asks) if self.asks else None


class BookSet:
    """All instruments; routes each ITCH message to its book by stock_locate."""
    def __init__(self):
        self.books = {}

    def apply(self, msg):
        b = self.books.get(msg.stock_locate)
        if b is None:
            b = self.books[msg.stock_locate] = Book(msg.stock_locate)
        b.apply(msg)
        return b


def _msg(t, ref, price, qty, side, inst=7):
    return wire.ItchMessage(t, inst, 0, 0, ref, qty, price, side)


def _selftest():
    b = Book(7)
    # rest two bids and one ask
    b.apply(_msg("A", 1, 100, 10, "B"))
    b.apply(_msg("A", 2, 101, 5, "B"))
    b.apply(_msg("A", 3, 200, 8, "S"))
    assert b.best_bid() == 101 and b.best_ask() == 200
    assert b.bids[100].quantity == 10 and b.bids[100].orders == 1

    # partial execute against ref 1, then finish it
    b.apply(_msg("E", 1, 100, 4, "B"))
    assert b.bids[100].quantity == 6
    b.apply(_msg("E", 1, 100, 6, "B"))
    assert 100 not in b.bids and 1 not in b.orders   # fully executed → level gone

    # execute for an unknown (aggressor) ref is ignored
    b.apply(_msg("E", 999, 200, 3, "S"))
    assert b.asks[200].quantity == 8

    # recycled id: cancel ref 2, then a fresh A reuses id 2 on the other side
    b.apply(_msg("X", 2, 101, 5, "B"))
    assert b.best_bid() is None                      # both bids gone
    b.apply(_msg("A", 2, 201, 9, "S"))
    assert b.orders[2] == ("S", 201, 9)              # fresh order, not the stale BUY@101
    assert b.best_ask() == 200 and 101 not in b.bids

    # BookSet routes by instrument
    bs = BookSet()
    bs.apply(_msg("A", 1, 50, 3, "B", inst=0))
    bs.apply(_msg("A", 1, 60, 4, "S", inst=1))
    assert bs.books[0].best_bid() == 50 and bs.books[1].best_ask() == 60
    print("orderbook: OK")


if __name__ == "__main__":
    _selftest()
