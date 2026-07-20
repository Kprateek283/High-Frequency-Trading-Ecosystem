"""Tier 5 — live TUI over the readers (rich). Decision #6: rich for the UI, the
readers below stay stdlib-only.

Four panels, each fed by one lower tier:
  health   ← feeds/stats_reader + core/metrics + health   (per-shard + heartbeat)
  tape     ← feeds/audit_reader                            (recent fills/cancels)
  book     ← feeds/multicast + core/orderbook              (top of book)
  liveness ← stats-region heartbeat age
The stats and audit readers are cheap and polled per frame. Market data is not:
it arrives in bursts far faster than the render rate, so it gets its own reader
thread (see _BookFeed) instead of being drained from the render loop.
"""
import threading
import time
from collections import deque

from rich.console import Console, Group
from rich.live import Live
from rich.panel import Panel
from rich.table import Table

from ..config import Config
from ..clock import Clock
from ..feeds.stats_reader import StatsReader
from ..feeds.audit_reader import AuditReader
from ..feeds.multicast import MulticastReader
from ..core import metrics
from ..core.orderbook import BookSet
from .. import wire, health


class _BookFeed:
    """Drains the ITCH socket on its own thread and keeps the books current.

    The render loop cannot do this job. It stops reading whenever it draws a
    frame or sleeps between frames, and market data arrives in bursts (a full
    cross is ~30k messages in under a second) that overflow the socket in that
    gap. One datagram carries one message and the wire has no sequence numbers,
    so the loss is silent: the reconstructed book simply drifts from the
    engine's (measured: thousands of phantom resting orders on a flat book).
    Reading continuously on a dedicated thread is what actually keeps up; the
    UI borrows a consistent view through `top_of_book`.
    """

    def __init__(self, mcast):
        self.mcast = mcast
        self.books = BookSet()
        self.applied = 0
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name="itch-reader", daemon=True)

    def start(self):
        self._thread.start()
        return self

    def _run(self):
        while not self._stop.is_set():
            try:
                for m in self.mcast.poll():
                    with self._lock:
                        self.books.apply(m)
                        self.applied += 1
            except OSError:
                continue          # nothing available this instant — keep reading

    def top_of_book(self, limit=10):
        """Snapshot of the first `limit` instruments, taken under the lock."""
        with self._lock:
            insts = sorted(self.books.books)[:limit]
            return [(i, self.books.books[i].best_bid(), self.books.books[i].best_ask())
                    for i in insts]

    def stop(self):
        self._stop.set()
        self._thread.join(timeout=2.0)


class Dashboard:
    def __init__(self, cfg=None, tape_size=12):
        self.cfg = cfg or Config()
        self.stats = None
        self.audit = None
        self.mcast = None
        self.feed = None          # _BookFeed once open_feeds() runs
        self.books = BookSet()    # used only when there is no live feed (tests)
        self.tape = deque(maxlen=tape_size)
        self.prev_snap = None
        self.prev_t = None
        self.window = {}          # last computed metrics.summarize()

    # --- feed wiring (each optional; a missing feed just leaves its panel waiting) ---
    def open_feeds(self):
        try:
            self.stats = StatsReader(self.cfg.get_path("STATS_SHM_PATH"))
        except OSError:
            self.stats = None
        try:
            self.audit = AuditReader(self.cfg.get_path("AUDIT_LOG_PATH"))
        except (OSError, ValueError):
            self.audit = None
        try:
            self.mcast = MulticastReader.from_config(self.cfg, timeout=0.05)
            self.feed = _BookFeed(self.mcast).start()
        except OSError:
            self.mcast = None

    def _poll(self):
        snap = self.stats.read() if self.stats else None
        if snap and self.prev_snap:
            dt = time.monotonic() - self.prev_t
            if dt > 0:
                self.window = metrics.summarize(self.prev_snap, snap, dt)
        if snap:
            self.prev_snap, self.prev_t = snap, time.monotonic()
        if self.audit:
            for e in self.audit.poll():
                self.tape.appendleft(e)
        # Market data is not polled here — _BookFeed's thread owns that socket.
        return snap

    # --- rendering ---
    def _health_panel(self, snap):
        t = Table(expand=True)
        # "rested/s" not "orders/s": orders_in counts NEW reports, and an order
        # that crosses on arrival never rests. There is no fill-ratio column
        # because these counters cannot define one (see core/metrics.trades).
        for col in ("shard", "rested/s", "trades/s", "trades", "eng_q", "pool_free", "state"):
            t.add_column(col, justify="right")
        rows = {r["shard"]: r for r in self.window.get("shards", [])}
        for i, sh in enumerate(snap.shards):
            w = rows.get(i, {})
            hr = metrics.pool_headroom(sh)
            t.add_row(str(i), f"{w.get('orders_per_s', 0):,.0f}",
                      f"{w.get('trades_per_s', 0):,.0f}",
                      f"{metrics.trades(sh):,.0f}", str(sh.engine_q_depth),
                      f"{hr*100:.0f}%", "")
        drops = (f"reports={snap.dropped_reports} drop_copies={snap.dropped_drop_copies}")
        return Panel(Group(t, f"[dim]drops:[/] {drops}"), title="Per-shard health")

    def _tape_panel(self):
        t = Table(expand=True)
        for col in ("state", "inst", "side", "px", "qty", "coid"):
            t.add_column(col, justify="right")
        for e in self.tape:
            t.add_row(wire.ORDER_STATE.get(e.state, "?"), str(e.instrument_id),
                      wire.SIDE.get(e.side, "?"), str(e.price), str(e.quantity),
                      str(e.client_order_id))
        return Panel(t, title="Trade tape (audit)")

    def _top_of_book(self, limit=10):
        if self.feed:
            return self.feed.top_of_book(limit)
        insts = sorted(self.books.books)[:limit]          # no live feed (tests)
        return [(i, self.books.books[i].best_bid(), self.books.books[i].best_ask())
                for i in insts]

    def _book_panel(self):
        t = Table(expand=True)
        for col in ("inst", "best_bid", "best_ask"):
            t.add_column(col, justify="right")
        for inst, bid, ask in self._top_of_book():
            t.add_row(str(inst), str(bid), str(ask))
        return Panel(t, title="Top of book (ITCH)")

    def _liveness_line(self, snap):
        if not snap:
            return "[yellow]waiting for stats region…[/]"
        clock = Clock(*snap.anchor)
        now_tsc = clock.now_tsc_estimate(time.time_ns())
        verdict = health.assess(snap, clock, now_tsc)
        age_ms = verdict["heartbeat_age_ns"] / 1e6
        colour = {"OK": "green", "WARN": "yellow", "CRITICAL": "red"}[verdict["overall"]]
        why = ("  " + "; ".join(verdict["reasons"])) if verdict["reasons"] else ""
        return f"[{colour}]● {verdict['overall']}[/]  heartbeat {age_ms:.0f}ms{why}"

    def render(self, snap):
        if not snap:
            return Panel("[yellow]No stats region — is the engine running?[/]", title="HFT monitor")
        return Group(self._liveness_line(snap), self._health_panel(snap),
                     self._book_panel(), self._tape_panel())

    def frame(self):
        """Poll every feed once and return one renderable (used by run() and tests)."""
        snap = self._poll()
        return self.render(snap)

    def run(self, duration=None, refresh_hz=4):
        self.open_feeds()
        console = Console()
        deadline = None if duration is None else time.monotonic() + duration
        with Live(self.frame(), console=console, refresh_per_second=refresh_hz,
                  screen=True) as live:
            while deadline is None or time.monotonic() < deadline:
                time.sleep(1.0 / refresh_hz)
                live.update(self.frame())

    def close(self):
        if self.feed:
            self.feed.stop()          # stop reading before the socket goes away
        for r in (self.stats, self.audit, self.mcast):
            if r:
                r.close()


def _render_once_headless():
    """Headless smoke: render a frame from a synthetic snapshot with no terminal.
    Proves the whole panel graph builds; the live gate exercises real feeds."""
    import struct
    from rich.console import Console
    d = Dashboard()
    # synthetic consistent region → one StatsSnapshot
    buf = bytearray(wire.STATS_REGION_SIZE)
    struct.pack_into("<III", buf, wire.OFF_MAGIC, wire.STATS_MAGIC, wire.PROTOCOL_VERSION, 4)
    struct.pack_into("<I", buf, wire.OFF_SEQ, 2)
    struct.pack_into(wire.ANCHOR_FMT, buf, wire.OFF_ANCHOR,
                     0, time.time_ns(), 2.0)
    struct.pack_into("<Q", buf, wire.OFF_HEARTBEAT_TSC, int(time.time_ns() * 2.0))
    struct.pack_into(wire.SHARD_FMT, buf, wire.SHARD_STATS_OFF, 5000, 10000, 0, 0, 3, 0, 0, 1000)
    from ..feeds.stats_reader import StatsSnapshot
    snap = StatsSnapshot(buf)
    d.books.apply(wire.ItchMessage("A", 0, 0, 0, 1, 100, 50000, "B"))
    d.tape.appendleft(wire.decode_audit_entry(_fake_entry()))
    console = Console(file=open("/dev/null", "w"), force_terminal=False)
    console.print(d.render(snap))     # must not raise
    return True


def _fake_entry():
    import struct
    e = bytearray(64)
    struct.pack_into("<Q", e, 0, 1)
    struct.pack_into(wire.DROPCOPY_FMT, e, wire.DROPCOPY_OFF, 42, 7, 50000, 100, 0, 2, 0)
    return e


def _feed_keeps_up_with_a_burst():
    """Regression: the book feed must absorb a burst far larger than one frame's
    worth. Draining from the render loop capped this at 64/frame and dropped the
    rest on the floor, silently desyncing the book."""
    n = 20000                                  # burst, delivered as fast as it can
    pending = [wire.ItchMessage("A", 0, 0, 0, ref, 10, 50000 + ref, "B")
               for ref in range(1, n + 1)]

    class BurstMcast:                          # one message per poll, then "empty"
        def poll(self):
            if not pending:
                raise BlockingIOError          # an OSError: nothing right now
            yield pending.pop(0)

    feed = _BookFeed(BurstMcast()).start()
    deadline = time.monotonic() + 10.0
    while feed.applied < n and time.monotonic() < deadline:
        feed.top_of_book()                     # hammer the lock like a render loop
        time.sleep(0.01)
    feed.stop()
    assert feed.applied == n, f"only {feed.applied}/{n} messages applied"
    assert len(feed.books.books[0].orders) == n
    return True


def _feed_stops_cleanly():
    """stop() must terminate the reader thread even on a feed that never idles."""
    class EndlessMcast:
        def poll(self):
            yield wire.ItchMessage("A", 0, 0, 0, 1, 10, 50000, "B")

    feed = _BookFeed(EndlessMcast()).start()
    time.sleep(0.05)
    feed.stop()
    assert not feed._thread.is_alive(), "reader thread outlived stop()"
    return True


def _selftest():
    assert _render_once_headless()
    assert _feed_keeps_up_with_a_burst()
    assert _feed_stops_cleanly()
    print("tui: OK (headless render, burst-proof feed, clean stop)")


if __name__ == "__main__":
    import sys
    if "--selftest" in sys.argv:
        _selftest()
    else:
        secs = None
        for a in sys.argv[1:]:
            if a.startswith("--seconds="):
                secs = float(a.split("=", 1)[1])
        Dashboard().run(duration=secs)
