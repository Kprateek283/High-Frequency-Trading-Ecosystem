// Book-only microbenchmark: OrderBook::match_order / cancel_order in-process.
//
// This measures the matching engine ALONE — no sockets, no gateway, no decode.
// It is deliberately separate from scripts/measure_throughput.py, which measures
// gateway ingest (decode + validate + enqueue) and never touches this code.
//
// It needs no SCHED_FIFO and no isolcpus, because it never blocks: there is no
// syscall in the timed region, so the only way the scheduler shows up is an
// involuntary preemption, which lands in the far tail. Read p50/p99 as real and
// treat p99.9+ on a shared box as an upper bound (the preemption, not the book).
//
// Every op is timed individually with rdtscp, so the queue drains between
// batches sit outside the measured window. Subtract the rdtscp baseline printed
// at the top from the per-op figures if you want the bare book cost.

#include "matching/orderbook.h"
#include "core/timer.h"

#include <cstdio>
#include <memory>
#include <random>
#include <vector>

// OrderBook::broadcast increments this on a dropped report; exchange.cpp owns
// the production definition, which this binary does not link.
EngineStats g_stats;

namespace {

constexpr uint32_t POOL_CAPACITY = POOL_CAPACITY_PER_SHARD;  // 500k, same as a production shard
constexpr uint32_t MID_PRICE = 50000;                        // centre of the 0..100000 tick domain
constexpr uint32_t DRAIN_EVERY = 65536;                      // « queue capacity, so pushes never fail

// Two identities: the book rejects a cross between orders of the same client
// (self-trade prevention), so makers and takers must be distinct or every
// match-path measurement would degenerate into the reject path.
constexpr uint32_t MAKER = 1;
constexpr uint32_t TAKER = 2;

// Owns everything one OrderBook needs. Far too big for the stack — the two Limit
// arrays alone are ~3.2 MB — hence the heap, same as the unit-test fixture.
struct Fixture {
    std::unique_ptr<MemoryPool<Order>> pool;
    std::unique_ptr<LockFreeQueue<ItchMessage, 1048576>> mkt_q;
    std::unique_ptr<LockFreeQueue<DropCopyMessage, 1048576>> dc_q;
    std::vector<Order*> orders_by_id;
    std::unique_ptr<OrderBook> book;

    Fixture()
        : pool(std::make_unique<MemoryPool<Order>>(POOL_CAPACITY)),
          mkt_q(std::make_unique<LockFreeQueue<ItchMessage, 1048576>>()),
          dc_q(std::make_unique<LockFreeQueue<DropCopyMessage, 1048576>>()),
          orders_by_id(MAX_ORDERS_LOOKUP, nullptr),
          book(std::make_unique<OrderBook>()) {
        book->init(pool.get(), mkt_q.get(), dc_q.get(), orders_by_id.data());
    }

    // Returns the pool-slot handle (== internal_id), or 0 if the pool is full.
    // 0 is the reserved null handle, so it is unambiguous as a failure value.
    uint32_t submit(Side side, uint32_t price, uint32_t qty, uint64_t token, uint32_t owner) {
        Order* o = pool->allocate(0, uint64_t{0}, token, owner, uint64_t{price}, qty,
                                  uint16_t{0}, side);
        if (!o) return 0;
        uint32_t id = pool->index_of(o);
        o->internal_id = id;
        book->match_order(o);
        return id;
    }

    // Same, but hands back the constructed order so a caller can time the
    // match_order() call on its own without the allocation inside the window.
    Order* prepare(Side side, uint32_t price, uint32_t qty, uint64_t token, uint32_t owner) {
        Order* o = pool->allocate(0, uint64_t{0}, token, owner, uint64_t{price}, qty,
                                  uint16_t{0}, side);
        if (o) o->internal_id = pool->index_of(o);
        return o;
    }

    void drain() {
        ItchMessage m;
        while (mkt_q->pop(m)) {}
        DropCopyMessage d;
        while (dc_q->pop(d)) {}
    }
};

void report(Timer& t, const char* label, uint64_t total_cycles, size_t ops) {
    t.printStats(label);
    const double ns = static_cast<double>(total_cycles) / t.cycles_per_ns_value();
    std::fprintf(stderr, "Throughput:       %.2f M ops/sec (%zu ops)\n",
                 static_cast<double>(ops) / ns * 1000.0, ops);
    t.clear();
}

// --- Scenario A: add, never crossing -----------------------------------------
// One-sided book, so match_order falls straight through to the insert path.
// Prices are normally distributed around the mid, which is what makes this
// interesting: repeated hits on the same level exercise the intrusive list,
// while the tails keep setting fresh bitmap bits.
void bench_add(Timer& t, size_t ops) {
    Fixture f;
    std::mt19937_64 rng(42);
    std::normal_distribution<double> price_dist(MID_PRICE, 50.0);

    std::vector<uint32_t> prices(ops);
    for (size_t i = 0; i < ops; ++i) {
        double p = price_dist(rng);
        prices[i] = static_cast<uint32_t>(p < 1.0 ? 1.0 : (p > MAX_PRICE - 2 ? MAX_PRICE - 2 : p));
    }

    uint64_t total = 0;
    size_t done = 0;
    for (size_t i = 0; i < ops; ++i) {
        Order* o = f.prepare(Side::BUY, prices[i], 100, i + 1, MAKER);
        if (!o) break;
        uint64_t s = get_tsc();
        f.book->match_order(o);
        uint64_t e = get_tsc();
        t.add_latency(e - s);
        total += e - s;
        ++done;
        if ((i & (DRAIN_EVERY - 1)) == 0) f.drain();
    }
    report(t, "A. add (no cross)", total, done);
}

// --- Scenario B: cancel -------------------------------------------------------
// Book is pre-built outside the window; the measured op is the O(1) unlink plus
// whatever bitmap/BBO repair emptying a level triggers.
void bench_cancel(Timer& t, size_t ops) {
    Fixture f;
    std::mt19937_64 rng(43);
    std::normal_distribution<double> price_dist(MID_PRICE, 50.0);

    std::vector<uint32_t> ids;
    ids.reserve(ops);
    for (size_t i = 0; i < ops; ++i) {
        double p = price_dist(rng);
        uint32_t price = static_cast<uint32_t>(p < 1.0 ? 1.0 : (p > MAX_PRICE - 2 ? MAX_PRICE - 2 : p));
        uint32_t id = f.submit(Side::BUY, price, 100, i + 1, MAKER);
        if (!id) break;
        ids.push_back(id);
        if ((i & (DRAIN_EVERY - 1)) == 0) f.drain();
    }
    f.drain();

    // Cancel in a shuffled order — sequential cancellation would walk the pool
    // linearly and hand the prefetcher a pattern production never sees.
    std::shuffle(ids.begin(), ids.end(), rng);

    uint64_t total = 0;
    for (size_t i = 0; i < ids.size(); ++i) {
        uint64_t s = get_tsc();
        f.book->cancel_order(ids[i]);
        uint64_t e = get_tsc();
        t.add_latency(e - s);
        total += e - s;
        if ((i & (DRAIN_EVERY - 1)) == 0) f.drain();
    }
    report(t, "B. cancel", total, ids.size());
}

// --- Scenario C: match, one fill per op ---------------------------------------
// Rest one ask per price level, then lift them from the bottom up. Each timed op
// is a single full fill that empties a level, so it also pays one find_next_ask
// bitmap scan — the cost that the L1/L2 ladder exists to make cheap.
void bench_match(Timer& t, size_t ops) {
    Fixture f;
    if (ops > MAX_PRICE - MID_PRICE - 2) ops = MAX_PRICE - MID_PRICE - 2;

    for (size_t i = 0; i < ops; ++i) {
        if (!f.submit(Side::SELL, MID_PRICE + static_cast<uint32_t>(i), 100, i + 1, MAKER)) {
            ops = i;
            break;
        }
        if ((i & (DRAIN_EVERY - 1)) == 0) f.drain();
    }
    f.drain();

    uint64_t total = 0;
    size_t done = 0;
    for (size_t i = 0; i < ops; ++i) {
        uint32_t price = MID_PRICE + static_cast<uint32_t>(i);
        Order* o = f.prepare(Side::BUY, price, 100, ops + i + 1, TAKER);
        if (!o) break;
        uint64_t s = get_tsc();
        f.book->match_order(o);
        uint64_t e = get_tsc();
        t.add_latency(e - s);
        total += e - s;
        ++done;
        if ((i & (DRAIN_EVERY - 1)) == 0) f.drain();
    }
    report(t, "C. match (1 fill/op)", total, done);
}

// --- Scenario D: mixed, HFT-shaped --------------------------------------------
// 70% add / 20% cancel / 10% aggressive cross, prices normal around the mid.
// This is the number to quote as "the book under realistic flow"; A-C are the
// decomposition that explains it.
void bench_mixed(Timer& t, size_t ops) {
    Fixture f;
    std::mt19937_64 rng(44);
    std::normal_distribution<double> price_dist(MID_PRICE, 50.0);
    std::uniform_int_distribution<int> op_dist(0, 99);

    std::vector<uint32_t> live;
    live.reserve(POOL_CAPACITY);
    size_t stale_cancels = 0, pool_full = 0;

    uint64_t total = 0;
    for (size_t i = 0; i < ops; ++i) {
        double pd = price_dist(rng);
        uint32_t price = static_cast<uint32_t>(pd < 1.0 ? 1.0 : (pd > MAX_PRICE - 2 ? MAX_PRICE - 2 : pd));
        int roll = op_dist(rng);

        if (roll < 20 && !live.empty()) {
            // Cancel a random live handle. A handle whose order already filled is
            // a no-op in the book — that is a real venue race (cancel loses to a
            // fill), so it is counted rather than filtered out.
            std::uniform_int_distribution<size_t> pick(0, live.size() - 1);
            size_t k = pick(rng);
            uint32_t id = live[k];
            live[k] = live.back();
            live.pop_back();
            if (!f.orders_by_id[id]) ++stale_cancels;

            uint64_t s = get_tsc();
            f.book->cancel_order(id);
            uint64_t e = get_tsc();
            t.add_latency(e - s);
            total += e - s;
        } else {
            // 10 of the remaining 80 points cross the book; the rest rest on it.
            bool aggressive = roll >= 90;
            Side side = (i & 1) ? Side::BUY : Side::SELL;
            uint32_t px = aggressive ? (side == Side::BUY ? price + 25 : (price > 25 ? price - 25 : 1))
                                     : price;
            Order* o = f.prepare(side, px, 100, i + 1, aggressive ? TAKER : MAKER);
            if (!o) { ++pool_full; continue; }
            uint32_t id = o->internal_id;

            uint64_t s = get_tsc();
            f.book->match_order(o);
            uint64_t e = get_tsc();
            t.add_latency(e - s);
            total += e - s;

            if (f.orders_by_id[id] == o) live.push_back(id);  // rested rather than filled
        }
        if ((i & (DRAIN_EVERY - 1)) == 0) f.drain();
    }
    report(t, "D. mixed 70/20/10", total, ops);
    std::fprintf(stderr, "  (resting at end: %zu, cancels that raced a fill: %zu, pool-full skips: %zu)\n",
                 live.size(), stale_cancels, pool_full);
}

// rdtscp is not free and it is inside every measurement above. Print what an
// empty timed region costs so the per-op numbers can be read net of it.
void bench_baseline(Timer& t, size_t ops) {
    uint64_t total = 0;
    for (size_t i = 0; i < ops; ++i) {
        uint64_t s = get_tsc();
        uint64_t e = get_tsc();
        t.add_latency(e - s);
        total += e - s;
    }
    report(t, "0. rdtscp baseline (empty region)", total, ops);
}

}  // namespace

int main(int argc, char** argv) {
    size_t ops = (argc > 1) ? std::strtoul(argv[1], nullptr, 10) : 400000;
    if (ops == 0 || ops > POOL_CAPACITY - 1) ops = POOL_CAPACITY - 1;

    Timer t(ops);
    std::fprintf(stderr, "book-only microbenchmark: %zu ops/scenario, TSC %.3f GHz\n",
                 ops, t.cycles_per_ns_value());

    bench_baseline(t, ops);
    bench_add(t, ops);
    bench_cancel(t, ops);
    bench_match(t, ops);
    bench_mixed(t, ops);
    return 0;
}
