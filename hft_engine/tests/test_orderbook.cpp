#include "tests.h"
#include "matching/orderbook.h"
#include <memory>
#include <vector>

// Bitmap-ladder boundary cases.
//
// The book tracks live price levels in a two-level bitmap: L1 has one bit per
// price (64 prices per uint64_t word), L2 has one bit per L1 word (so one L2
// word covers 64*64 = 4096 prices). find_next_ask/find_next_bid walk it in
// three steps: the remainder of the current L1 word, the rest of the current
// L2 group, then following L2 groups. The step boundaries are where a resolved
// CRITICAL previously lived, so each step is exercised separately below.
//
// The scan functions are private and the BBO cache is private, so these tests
// observe them the way production does: through match_order/cancel_order and
// the ITCH messages the book broadcasts. A price level that the scan fails to
// find is a price that does not fill — which is exactly what is asserted.

namespace {

// Owns everything one OrderBook needs. All of it is far too big for the stack
// (the two Limit arrays alone are ~3.2 MB), hence the heap.
struct BookFixture {
    std::unique_ptr<MemoryPool<Order>> pool;
    std::unique_ptr<LockFreeQueue<ItchMessage, 1048576>> mkt_q;
    std::unique_ptr<LockFreeQueue<DropCopyMessage, 1048576>> dc_q;
    std::vector<Order*> orders_by_id;
    std::unique_ptr<OrderBook> book;

    BookFixture()
        : pool(std::make_unique<MemoryPool<Order>>(4096)),
          mkt_q(std::make_unique<LockFreeQueue<ItchMessage, 1048576>>()),
          dc_q(std::make_unique<LockFreeQueue<DropCopyMessage, 1048576>>()),
          orders_by_id(MAX_ORDERS_LOOKUP, nullptr),
          book(std::make_unique<OrderBook>()) {
        book->init(pool.get(), mkt_q.get(), dc_q.get(), orders_by_id.data());
    }

    // Submits an order and returns its pool-slot handle (== its internal_id).
    // `token` is the client's order token; `owner` is the client identity, which
    // defaults to distinct-per-token so these tests keep their existing
    // "every order is a different client" behaviour.
    uint32_t submit(Side side, uint32_t price, uint32_t qty, uint64_t token,
                    uint32_t owner = 0) {
        if (owner == 0) owner = static_cast<uint32_t>(token);
        Order* o = pool->allocate(0, token, owner, price, qty, uint16_t{0}, side);
        uint32_t id = pool->index_of(o);
        o->internal_id = id;
        book->match_order(o);
        return id;
    }

    void drain() {
        ItchMessage m;
        while (mkt_q->pop(m)) {}
        DropCopyMessage d;
        while (dc_q->pop(d)) {}
    }

    // Counts ITCH executions ('E') emitted since the last drain.
    int count_execs() {
        int execs = 0;
        ItchMessage m;
        while (mkt_q->pop(m)) {
            if (m.msg_type == 'E') ++execs;
        }
        return execs;
    }
};

} // namespace

void test_orderbook() {
    // --- Empty book: nothing to match against, the order just rests. ---
    {
        BookFixture f;
        f.submit(Side::BUY, 50000, 100, 1);
        int execs = f.count_execs();
        CHECK(execs == 0);  // no counterparty
    }

    // --- Simple cross at the same price. ---
    {
        BookFixture f;
        f.submit(Side::SELL, 50000, 100, 1);
        f.drain();
        f.submit(Side::BUY, 50000, 100, 2);
        // One execution per side of the trade.
        CHECK(f.count_execs() == 2);
    }

    // --- A buy below the best ask must not fill. ---
    {
        BookFixture f;
        f.submit(Side::SELL, 50000, 100, 1);
        f.drain();
        f.submit(Side::BUY, 49999, 100, 2);
        CHECK(f.count_execs() == 0);
    }

    // --- L1 word boundary (step 1 of find_next_ask). ---
    // Prices 63 and 64 sit in adjacent L1 words. Filling the level at 63 clears
    // its bit and forces a rescan that must cross into the next word.
    {
        BookFixture f;
        f.submit(Side::SELL, 63, 100, 1);
        f.submit(Side::SELL, 64, 100, 2);
        f.drain();

        f.submit(Side::BUY, 63, 100, 3);      // consumes the 63 level entirely
        CHECK(f.count_execs() == 2);

        f.submit(Side::BUY, 64, 100, 4);      // best ask must now be 64
        CHECK(f.count_execs() == 2);
    }

    // --- L2 group boundary, ask side (steps 2 and 3 of find_next_ask). ---
    // Price 4032 is bit 0 of L1 word 63, which is bit 63 of L2 word 0 — the
    // exact case the (bit_in_l2 == 63) shift-by-64 guard exists for. The next
    // ask at 8192 lives in L2 word 2, so the rescan has to fall through the
    // "rest of this L2 group" step into the "following L2 groups" loop.
    {
        BookFixture f;
        f.submit(Side::SELL, 4032, 100, 1);
        f.submit(Side::SELL, 8192, 100, 2);
        f.drain();

        f.submit(Side::BUY, 4032, 100, 3);    // clears 4032, rescan must find 8192
        CHECK(f.count_execs() == 2);

        f.submit(Side::BUY, 8191, 100, 4);    // below 8192: must not fill
        CHECK(f.count_execs() == 0);

        f.submit(Side::BUY, 8192, 100, 5);    // at 8192: must fill
        CHECK(f.count_execs() == 2);
    }

    // --- L2 group boundary, bid side (the (bit_in_l2 == 0) guard). ---
    // Price 4096 is bit 0 of L1 word 64, which is bit 0 of L2 word 1. Clearing
    // it forces the bid rescan to walk down out of L2 word 1 into word 0.
    {
        BookFixture f;
        f.submit(Side::BUY, 4096, 100, 1);
        f.submit(Side::BUY, 100, 100, 2);
        f.drain();

        f.submit(Side::SELL, 4096, 100, 3);   // clears 4096, rescan must find 100
        CHECK(f.count_execs() == 2);

        f.submit(Side::SELL, 101, 100, 4);    // above 100: must not fill
        CHECK(f.count_execs() == 0);

        f.submit(Side::SELL, 100, 100, 5);    // at 100: must fill
        CHECK(f.count_execs() == 2);
    }

    // --- Cancel drives the same rescan path as a fill. ---
    {
        BookFixture f;
        uint32_t id_hi = f.submit(Side::SELL, 4032, 100, 1);
        f.submit(Side::SELL, 8192, 100, 2);
        f.drain();

        f.book->cancel_order(id_hi);          // clears the best ask by cancel
        f.drain();

        f.submit(Side::BUY, 8192, 100, 3);    // best ask must have rescanned to 8192
        CHECK(f.count_execs() == 2);
    }

    // --- Price-time priority within one level. ---
    {
        BookFixture f;
        f.submit(Side::SELL, 500, 100, 11);   // first in
        f.submit(Side::SELL, 500, 100, 22);   // second in
        f.drain();

        f.submit(Side::BUY, 500, 100, 33);    // must hit client 11's order

        bool matched_first = false;
        DropCopyMessage d;
        while (f.dc_q->pop(d)) {
            if (d.client_order_id == 11 && d.state == OrderState::FILLED) {
                matched_first = true;
            }
            CHECK(d.client_order_id != 22);   // the later order must be untouched
        }
        CHECK(matched_first);
    }

    // --- Partial fill leaves the remainder resting. ---
    {
        BookFixture f;
        f.submit(Side::SELL, 700, 100, 1);
        f.drain();

        f.submit(Side::BUY, 700, 30, 2);      // takes 30 of 100
        CHECK(f.count_execs() == 2);

        f.submit(Side::BUY, 700, 70, 3);      // the remaining 70 must still be there
        CHECK(f.count_execs() == 2);

        f.submit(Side::BUY, 700, 1, 4);       // level is now empty: no fill
        CHECK(f.count_execs() == 0);
    }

    // --- A sweep across several price levels in one order. ---
    {
        BookFixture f;
        f.submit(Side::SELL, 100, 10, 1);
        f.submit(Side::SELL, 101, 10, 2);
        f.submit(Side::SELL, 102, 10, 3);
        f.drain();

        f.submit(Side::BUY, 102, 30, 4);      // must clear all three levels
        CHECK(f.count_execs() == 6);          // 3 trades, 2 messages each

        f.submit(Side::BUY, 102, 1, 5);       // book side is empty now
        CHECK(f.count_execs() == 0);
    }

    // --- Drop-copy loss is counted, not silent (Phase 2.2). ---
    // Fill the drop-copy queue to capacity, then submit: the order's NEW
    // drop-copy can't be pushed, and the dropped_drop_copies counter must move.
    {
        BookFixture f;
        uint64_t before = g_stats.dropped_drop_copies.load();
        DropCopyMessage full{};
        while (f.dc_q->push(full)) {}         // saturate the queue
        f.submit(Side::BUY, 50000, 100, 1);   // NEW drop-copy is dropped
        CHECK(g_stats.dropped_drop_copies.load() > before);
    }
}
