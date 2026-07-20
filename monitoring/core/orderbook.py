"""Tier 2 — reconstruct per-instrument L2 books from the ITCH stream (I1).

Order-reference ids recycle: the engine's internal_id is a pool slot index, freed
when an order leaves the book and reused by the next `A`. So an `A` is
authoritative — it starts a fresh order for that id, overwriting any stale one.
Engine ITCH semantics (matching/orderbook.h broadcast()):
  'A' ref rests: (side, price, shares)      — only the leftover after matching
  'E' ref executes `shares` at `price`      — emitted for both resting & aggressor;
      the aggressor's ref never rested, so an 'E' for an unknown ref is ignored
  'X' ref removed

Market data is UDP with no sequence numbers, so some loss is unavoidable and
undetectable. An order whose 'E' or 'X' is lost would otherwise rest here
forever: memory grows without bound, and a phantom at a better price than the
live market permanently shadows the true top of book. Two mechanisms bound that
damage — see `evict_stale` (general) and `repair_crossed` (immediate).
"""
import time

from .. import wire
from ..models import BookLevel

# How long an untouched order may rest before we assume its removal was lost.
# The trade-off is real in both directions: too short evicts genuinely
# long-resting orders, too long leaves a phantom shadowing top of book. Orders in
# this system churn in seconds (see tools/liquidity.cpp, tools/tester.cpp), so
# minutes is generous. Raise it for a venue with long-lived resting interest.
DEFAULT_ORDER_TTL_S = 120.0


class Book:
    """One instrument's book: resting orders + aggregated price levels."""
    def __init__(self, instrument_id):
        self.instrument_id = instrument_id
        self.orders = {}                 # ref -> (side_char, price, remaining_qty, touched_at)
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
        side_char, price, remaining, _ = self.orders.pop(ref)
        self._level_sub(side_char, price, remaining, drop_order=True)

    def apply(self, msg, now=None):
        now = time.monotonic() if now is None else now
        t = msg.msg_type
        ref = msg.internal_id
        if t == "A":
            if ref in self.orders:               # recycled id still on the books: drop the stale one
                self._remove_order(ref)
            self.orders[ref] = (msg.side, msg.price, msg.shares, now)
            self._level_add(msg.side, msg.price, msg.shares)
        elif t == "E":
            o = self.orders.get(ref)
            if o is None:
                return                            # aggressor ref never rested — nothing to reduce
            side_char, price, remaining, _ = o
            remaining -= msg.shares
            if remaining <= 0:
                self._remove_order(ref)
            else:
                self.orders[ref] = (side_char, price, remaining, now)
                self._level_sub(side_char, price, msg.shares, drop_order=False)
        elif t == "X":
            if ref in self.orders:
                self._remove_order(ref)

    def evict_stale(self, now, ttl=DEFAULT_ORDER_TTL_S):
        """Drop orders untouched for longer than `ttl`; returns how many went.

        This is the general backstop for a lost 'E'/'X'. Without it the book only
        ever grows, and every lost removal is permanent.
        """
        stale = [ref for ref, (_, _, _, touched) in self.orders.items()
                 if now - touched > ttl]
        for ref in stale:
            self._remove_order(ref)
        return len(stale)

    def repair_crossed(self, max_evictions=64):
        """Uncross the book by evicting the oldest order on the crossing levels.

        A real book cannot cross: the engine would have matched those orders
        instead of resting them. So a crossed reconstruction is proof that at
        least one resting order here is a phantom left by a lost message. The
        phantom is necessarily older than the live market that crossed it, so
        evicting oldest-first targets it rather than real interest.

        This handles the visible harm immediately, rather than waiting out the
        TTL with a wrong top of book on screen. Bounded so a pathological book
        cannot spin. O(orders) per eviction, which is fine because a crossed book
        is rare — if that stops being true, the drop rate is the real problem.
        """
        evicted = 0
        while evicted < max_evictions:
            bid, ask = self.best_bid(), self.best_ask()
            if bid is None or ask is None or bid < ask:
                return evicted
            crossing = [(touched, ref) for ref, (side, price, _, touched) in self.orders.items()
                        if (side == "B" and price >= ask) or (side == "S" and price <= bid)]
            if not crossing:
                return evicted
            self._remove_order(min(crossing)[1])
            evicted += 1
        return evicted

    def best_bid(self):
        return max(self.bids) if self.bids else None

    def best_ask(self):
        return min(self.asks) if self.asks else None


class BookSet:
    """All instruments; routes each ITCH message to its book by stock_locate."""
    def __init__(self):
        self.books = {}

    def apply(self, msg, now=None):
        b = self.books.get(msg.stock_locate)
        if b is None:
            b = self.books[msg.stock_locate] = Book(msg.stock_locate)
        b.apply(msg, now)
        return b

    def evict_stale(self, now, ttl=DEFAULT_ORDER_TTL_S):
        """Sweep every instrument; returns the total evicted."""
        return sum(b.evict_stale(now, ttl) for b in self.books.values())

    def repair_crossed(self):
        """Repair any crossed book; returns the total evicted."""
        return sum(b.repair_crossed() for b in self.books.values())


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
    assert b.orders[2][:3] == ("S", 201, 9)          # fresh order, not the stale BUY@101
    assert b.best_ask() == 200 and 101 not in b.bids

    # BookSet routes by instrument
    bs = BookSet()
    bs.apply(_msg("A", 1, 50, 3, "B", inst=0))
    bs.apply(_msg("A", 1, 60, 4, "S", inst=1))
    assert bs.books[0].best_bid() == 50 and bs.books[1].best_ask() == 60

    # --- D4: a lost removal must not strand an order forever ---

    # TTL backstop: an order whose 'E'/'X' never arrives ages out, a fresh one stays.
    b = Book(7)
    b.apply(_msg("A", 1, 100, 10, "B"), now=0.0)      # its removal will be "lost"
    b.apply(_msg("A", 2, 99, 10, "B"), now=1000.0)    # fresh
    assert b.evict_stale(now=1000.0, ttl=120.0) == 1
    assert 1 not in b.orders and 2 in b.orders
    assert b.best_bid() == 99                        # phantom no longer shadows the top

    # Crossed-book repair: the engine can never rest a crossed book, so a cross
    # proves a phantom. It must be removed immediately, not after the TTL.
    b = Book(7)
    b.apply(_msg("A", 1, 50500, 10, "B"), now=0.0)    # stale phantom bid, too high
    b.apply(_msg("A", 2, 50000, 10, "S"), now=500.0)  # live ask below it => crossed
    assert b.best_bid() == 50500 and b.best_ask() == 50000
    assert b.repair_crossed() == 1
    assert 1 not in b.orders and 2 in b.orders       # the OLD order went, not the live one
    assert b.best_bid() is None and b.best_ask() == 50000

    # A legitimately uncrossed book must never be touched by the repair.
    b = Book(7)
    b.apply(_msg("A", 1, 99, 10, "B"), now=0.0)
    b.apply(_msg("A", 2, 101, 10, "S"), now=0.0)
    assert b.repair_crossed() == 0 and len(b.orders) == 2
    print("orderbook: OK")


if __name__ == "__main__":
    _selftest()
