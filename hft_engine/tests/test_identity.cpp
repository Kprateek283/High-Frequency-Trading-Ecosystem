#include "gateway_fixture.h"

// Server-side client identity and order-token handling (review A6).
//
// The gateway used to build its "client_id" by walking the 14-byte order_token
// and keeping whatever digits it found. That conflated two different things —
// which ORDER this is, and which CLIENT sent it — into one client-controlled
// field. Two structurally different tokens ("ABC00001", "00001XYZ") collapsed
// to the same id, and nothing stopped one client from naming another's order.
//
// Now: the token identifies the order and must be 14 digits; identity is
// assigned from the connection.

using namespace gwtest;

void test_identity() {
    // --- The order token is echoed through as the order id, unchanged. ---
    {
        GatewayFixture f;
        f.connect(7);
        OuchEnterOrder o = make_order(12345, 50000, 100, 'B');
        f.send_bytes(&o, sizeof(o));
        f.pump();

        auto tasks = f.drain_tasks();
        CHECK(tasks.size() == 1);
        CHECK(tasks[0].order->client_order_id == 12345);
    }

    // --- Identity comes from the connection, not from the token. ---
    // Same token bytes on two different connections must produce two orders
    // owned by two different clients. Under the old scheme both would have
    // reported the same "client".
    {
        GatewayFixture f;
        f.connect(11);
        f.connect(22);

        OuchEnterOrder a = make_order(500, 50000, 100, 'B');
        f.send_bytes(&a, sizeof(a), 0);
        f.pump(0);
        auto first = f.drain_tasks();
        CHECK(first.size() == 1);
        CHECK(first[0].order->client_id == 11);

        OuchEnterOrder b = make_order(501, 50000, 100, 'B');
        f.send_bytes(&b, sizeof(b), 1);
        f.pump(1);
        auto second = f.drain_tasks();
        CHECK(second.size() == 1);
        CHECK(second[0].order->client_id == 22);

        // The identity is the connection's, and the two differ even though the
        // token numbers are adjacent and the payloads otherwise identical.
        CHECK(first[0].order->client_id != second[0].order->client_id);
    }

    // --- A client cannot cancel an order it does not own. ---
    // This is A6's headline impact: cross-client cancel attribution.
    {
        GatewayFixture f;
        f.connect(11);   // owner
        f.connect(22);   // impostor

        OuchEnterOrder o = make_order(900, 50000, 100, 'B');
        f.send_bytes(&o, sizeof(o), 0);
        f.pump(0);
        CHECK(f.tasks_enqueued() == 1);   // the order is live and registered

        // Connection 22 names connection 11's token.
        OuchCancelOrder c = make_cancel(900);
        f.send_bytes(&c, sizeof(c), 1);
        f.pump(1);
        CHECK(f.tasks_enqueued() == 0);   // must not resolve

        // The real owner cancels the same token successfully.
        f.send_bytes(&c, sizeof(c), 0);
        f.pump(0);
        CHECK(f.tasks_enqueued() == 1);
    }

    // --- A non-numeric token is rejected, not silently collapsed. ---
    // "ABC00001" and "00001XYZ" both used to decode to 1 and become the same
    // order. Both must now reject.
    {
        GatewayFixture f;
        f.connect(1);

        for (const char* bad_token : {"ABC00001      ", "00001XYZ      ",
                                      "0000000000001 ", "1             ",
                                      "              "}) {
            OuchEnterOrder o = make_order(1, 50000, 100, 'B');
            std::memcpy(o.order_token, bad_token, 14);
            f.send_bytes(&o, sizeof(o), 0);
            f.pump(0);
            CHECK(f.tasks_enqueued() == 0);    // never reaches the engine
            CHECK(f.rejects_enqueued() == 1);  // and is reported as a reject
        }
    }

    // --- A token beyond the session map's range is rejected up front. ---
    // Such an order could never be registered, so its cancel could never
    // resolve; accepting it would create an uncancellable resting order.
    {
        GatewayFixture f;
        f.connect(1);
        CHECK(SessionManager::MAX_CLIENT_ORDERS == 50000000);

        OuchEnterOrder o = make_order(50000000, 50000, 100, 'B');  // exactly at the cap
        f.send_bytes(&o, sizeof(o));
        f.pump();
        CHECK(f.tasks_enqueued() == 0);
        CHECK(f.rejects_enqueued() == 1);

        OuchEnterOrder ok = make_order(49999999, 50000, 100, 'B'); // last valid
        f.send_bytes(&ok, sizeof(ok));
        f.pump();
        CHECK(f.tasks_enqueued() == 1);
    }

    // --- A rejected order still reports its token back when it has one. ---
    {
        GatewayFixture f;
        f.connect(1);
        OuchEnterOrder o = make_order(4242, 50000, 0, 'B');   // shares == 0 -> risk reject
        f.send_bytes(&o, sizeof(o));
        f.pump();
        auto rejects = f.drain_rejects();
        CHECK(rejects.size() == 1);
        CHECK(rejects[0].client_order_id == 4242);
        CHECK(rejects[0].state == OrderState::REJECTED);
    }

    // --- A malformed token has no order id to echo, so it reports 0. ---
    {
        GatewayFixture f;
        f.connect(1);
        OuchEnterOrder o = make_order(1, 50000, 100, 'B');
        std::memcpy(o.order_token, "NOT-A-NUMBER!!", 14);
        f.send_bytes(&o, sizeof(o));
        f.pump();
        auto rejects = f.drain_rejects();
        CHECK(rejects.size() == 1);
        CHECK(rejects[0].client_order_id == 0);
    }

    // --- The session map round-trips all three packed fields. ---
    // internal_id, instrument_id and client_id share one atomic word; this
    // pins the packing rather than trusting the shift arithmetic.
    {
        SessionManager sm;
        sm.record_order(4242, 500000, 255, 0xDEADBEEF);
        OrderSessionData d = sm.lookup_data(4242);
        CHECK(d.internal_id == 500000);
        CHECK(d.instrument_id == 255);
        CHECK(d.client_id == 0xDEADBEEF);

        // An unregistered token reads back as "no order".
        CHECK(sm.lookup_data(4243).internal_id == 0);

        // The boundary values of each field.
        sm.record_order(1, SessionManager::MAX_INTERNAL_ID - 1, MAX_INSTRUMENTS - 1, 0xFFFFFFFF);
        OrderSessionData e = sm.lookup_data(1);
        CHECK(e.internal_id == SessionManager::MAX_INTERNAL_ID - 1);
        CHECK(e.instrument_id == MAX_INSTRUMENTS - 1);
        CHECK(e.client_id == 0xFFFFFFFF);

        sm.record_order(2, 1, 0, 1);
        OrderSessionData g = sm.lookup_data(2);
        CHECK(g.internal_id == 1);
        CHECK(g.instrument_id == 0);
        CHECK(g.client_id == 1);
    }

    // --- Gateway core-map parse (Phase 3.2). ---
    // core_for_worker picks the idx-th entry of the GATEWAY_CORES list and returns
    // -1 for unset/empty/short lists so pin_gateway_worker leaves the worker alone.
    {
        CHECK(core_for_worker("1,3,5,7", 0) == 1);
        CHECK(core_for_worker("1,3,5,7", 3) == 7);
        CHECK(core_for_worker("1,3,5,7", 4) == -1);  // past the end
        CHECK(core_for_worker("2", 1) == -1);        // fewer cores than workers
        CHECK(core_for_worker("", 0) == -1);
        CHECK(core_for_worker(nullptr, 0) == -1);
    }
}
