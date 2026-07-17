#include "gateway_fixture.h"
#include <algorithm>

// TCP framing and buffer compaction.
//
// OUCH messages are fixed-size but TCP is a byte stream: a read can deliver
// half a message, or two and a half. The gateway keeps a per-connection buffer
// and compacts the unparsed fragment to the front before each read. A previous
// bug hard-reset that buffer instead, discarding partial messages and desyncing
// the stream — these tests exist so that cannot come back silently.

using namespace gwtest;

void test_framing() {
    // --- One whole message in one write. The baseline. ---
    {
        GatewayFixture f;
        f.connect(1);
        OuchEnterOrder o = make_order(1, 50000, 100, 'B');
        f.send_bytes(&o, sizeof(o));
        f.pump();
        CHECK(f.tasks_enqueued() == 1);
    }

    // --- One message split across two reads. ---
    // The first pump sees 40 of 81 bytes and must enqueue nothing while
    // retaining the fragment; the second must complete it.
    {
        GatewayFixture f;
        f.connect(1);
        OuchEnterOrder o = make_order(2, 50000, 100, 'B');
        const char* p = reinterpret_cast<const char*>(&o);

        f.send_bytes(p, 40);
        f.pump();
        CHECK(f.tasks_enqueued() == 0);   // incomplete: nothing enqueued yet

        f.send_bytes(p + 40, sizeof(o) - 40);
        f.pump();
        CHECK(f.tasks_enqueued() == 1);   // fragment was kept, not discarded
    }

    // --- Byte-at-a-time delivery: the pathological split. ---
    {
        GatewayFixture f;
        f.connect(1);
        OuchEnterOrder o = make_order(3, 50000, 100, 'B');
        const char* p = reinterpret_cast<const char*>(&o);
        for (size_t i = 0; i < sizeof(o); ++i) {
            f.send_bytes(p + i, 1);
            f.pump();
            if (i < sizeof(o) - 1) CHECK(f.tasks_enqueued() == 0);
        }
        CHECK(f.tasks_enqueued() == 1);
    }

    // --- Two whole messages in one write. ---
    {
        GatewayFixture f;
        f.connect(1);
        OuchEnterOrder a = make_order(4, 50000, 100, 'B');
        OuchEnterOrder b = make_order(5, 50001, 100, 'S');
        char buf[sizeof(a) * 2];
        std::memcpy(buf, &a, sizeof(a));
        std::memcpy(buf + sizeof(a), &b, sizeof(b));
        f.send_bytes(buf, sizeof(buf));
        f.pump();
        CHECK(f.tasks_enqueued() == 2);
    }

    // --- Compaction: a whole message plus a fragment of the next. ---
    // This is the exact shape the old hard-reset got wrong. After parsing the
    // first message, read_pos > 0 and a 40-byte fragment sits behind it; that
    // fragment must be memmove'd to the front and survive to the next read.
    {
        GatewayFixture f;
        f.connect(1);
        OuchEnterOrder a = make_order(6, 50000, 100, 'B');
        OuchEnterOrder b = make_order(7, 50001, 100, 'S');

        char buf[sizeof(a) + 40];
        std::memcpy(buf, &a, sizeof(a));
        std::memcpy(buf + sizeof(a), &b, 40);
        f.send_bytes(buf, sizeof(buf));
        f.pump();
        CHECK(f.tasks_enqueued() == 1);   // only the complete one

        f.send_bytes(reinterpret_cast<const char*>(&b) + 40, sizeof(b) - 40);
        f.pump();
        CHECK(f.tasks_enqueued() == 1);   // the compacted fragment completed
    }

    // --- A long run of messages split at an offset that never aligns. ---
    // 81-byte messages delivered in 50-byte chunks: every message boundary
    // lands mid-chunk, so compaction runs on essentially every pass.
    {
        GatewayFixture f;
        f.connect(1);
        constexpr int N = 20;
        char buf[sizeof(OuchEnterOrder) * N];
        for (int i = 0; i < N; ++i) {
            OuchEnterOrder o = make_order(100 + i, 50000, 100, (i % 2) ? 'S' : 'B');
            std::memcpy(buf + i * sizeof(o), &o, sizeof(o));
        }

        int total = 0;
        size_t sent = 0;
        while (sent < sizeof(buf)) {
            size_t chunk = std::min<size_t>(50, sizeof(buf) - sent);
            f.send_bytes(buf + sent, chunk);
            sent += chunk;
            f.pump();
            total += f.tasks_enqueued();
        }
        CHECK(total == N);   // every message decoded exactly once
    }

    // --- A cancel is 19 bytes, not 81: mixed sizes must stay in frame. ---
    {
        GatewayFixture f;
        f.connect(1);
        OuchEnterOrder o = make_order(8, 50000, 100, 'B');
        f.send_bytes(&o, sizeof(o));
        f.pump();
        CHECK(f.tasks_enqueued() == 1);

        OuchCancelOrder c = make_cancel(8);
        OuchEnterOrder o2 = make_order(9, 50000, 100, 'B');
        char buf[sizeof(c) + sizeof(o2)];
        std::memcpy(buf, &c, sizeof(c));
        std::memcpy(buf + sizeof(c), &o2, sizeof(o2));
        f.send_bytes(buf, sizeof(buf));
        f.pump();
        // The cancel resolves against the live order from above, and the order
        // after it must still be framed correctly.
        CHECK(f.tasks_enqueued() == 2);
    }

    // --- An unknown message type drops the connection. ---
    // Framing cannot resync from an unknown type: the size is unknown, so
    // every following byte is suspect.
    {
        GatewayFixture f;
        f.connect(1);
        char junk = 'Z';
        f.send_bytes(&junk, 1);
        f.pump();
        CHECK(f.tasks_enqueued() == 0);
        CHECK(f.states.find(f.conns[0].server_fd) == f.states.end());  // dropped
        f.conns[0].server_fd = -1;  // handle_client closed it
    }

    // --- A risk-rejected order must not desync the stream. ---
    // shares == 0 fails the pre-trade check; the message is still 81 bytes and
    // the order behind it must decode normally.
    {
        GatewayFixture f;
        f.connect(1);
        OuchEnterOrder bad = make_order(10, 50000, 0, 'B');   // shares == 0 -> reject
        OuchEnterOrder good = make_order(11, 50000, 100, 'B');
        char buf[sizeof(bad) + sizeof(good)];
        std::memcpy(buf, &bad, sizeof(bad));
        std::memcpy(buf + sizeof(bad), &good, sizeof(good));
        f.send_bytes(buf, sizeof(buf));
        f.pump();
        CHECK(f.rejects_enqueued() == 1);
        CHECK(f.tasks_enqueued() == 1);
    }
}
