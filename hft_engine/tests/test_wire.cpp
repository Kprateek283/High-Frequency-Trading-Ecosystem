#include "tests.h"
#include "protocol/messages.h"
#include "matching/order.h"
#include "auxiliary/order_manager.h"
#include <cstddef>

// The wire layout is asserted at compile time in messages.h. These runtime
// checks exist for a different reader: monitoring/wire.py (Phase 5) mirrors
// these exact sizes and offsets in Python, and its test asserts the same
// numbers. Writing them out here means the two languages are pinned to one
// list, and a struct change that slips past a stale wire.py still fails here.
void test_wire() {
    CHECK(PROTOCOL_VERSION == 1);

    CHECK(sizeof(OuchEnterOrder) == 81);
    CHECK(sizeof(OuchCancelOrder) == 19);
    CHECK(sizeof(OuchExecutionReport) == 32);
    CHECK(sizeof(ItchMessage) == 34);

    // OuchEnterOrder field offsets — packed, so every field is byte-exact.
    CHECK(offsetof(OuchEnterOrder, msg_type) == 0);
    CHECK(offsetof(OuchEnterOrder, order_token) == 1);
    CHECK(offsetof(OuchEnterOrder, side) == 15);
    CHECK(offsetof(OuchEnterOrder, shares) == 16);
    CHECK(offsetof(OuchEnterOrder, stock) == 20);
    CHECK(offsetof(OuchEnterOrder, price) == 28);
    CHECK(offsetof(OuchEnterOrder, time_in_force) == 32);
    CHECK(offsetof(OuchEnterOrder, firm) == 36);
    CHECK(offsetof(OuchEnterOrder, display) == 40);
    CHECK(offsetof(OuchEnterOrder, capacity) == 41);
    CHECK(offsetof(OuchEnterOrder, iso_eligibility) == 42);
    CHECK(offsetof(OuchEnterOrder, min_quantity) == 43);
    CHECK(offsetof(OuchEnterOrder, cross_type) == 47);
    CHECK(offsetof(OuchEnterOrder, customer_type) == 48);
    CHECK(offsetof(OuchEnterOrder, t1_exchange_send) == 49);
    CHECK(offsetof(OuchEnterOrder, t2_trading_recv) == 57);
    CHECK(offsetof(OuchEnterOrder, t3_trading_enq) == 65);
    CHECK(offsetof(OuchEnterOrder, t4_network_deq) == 73);

    CHECK(offsetof(OuchCancelOrder, msg_type) == 0);
    CHECK(offsetof(OuchCancelOrder, order_token) == 1);
    CHECK(offsetof(OuchCancelOrder, shares) == 15);

    CHECK(offsetof(ItchMessage, msg_type) == 0);
    CHECK(offsetof(ItchMessage, stock_locate) == 1);
    CHECK(offsetof(ItchMessage, tracking_number) == 3);
    CHECK(offsetof(ItchMessage, timestamp) == 5);
    CHECK(offsetof(ItchMessage, internal_id) == 13);
    CHECK(offsetof(ItchMessage, shares) == 21);
    CHECK(offsetof(ItchMessage, price) == 25);
    CHECK(offsetof(ItchMessage, side) == 33);

    // The on-disk audit format. OrderLogEntry is deliberately NOT packed: its
    // alignment padding is part of the format Phase 5's audit_reader.py must
    // reproduce byte-for-byte, so the padding is pinned here as a real number
    // rather than left as an accident of the compiler's choices.
    CHECK(sizeof(DropCopyMessage) == 32);
    CHECK(alignof(DropCopyMessage) == 32);
    CHECK(offsetof(DropCopyMessage, client_order_id) == 0);
    CHECK(offsetof(DropCopyMessage, internal_id) == 8);
    CHECK(offsetof(DropCopyMessage, price) == 16);
    CHECK(offsetof(DropCopyMessage, quantity) == 24);
    CHECK(offsetof(DropCopyMessage, instrument_id) == 28);
    CHECK(offsetof(DropCopyMessage, state) == 30);
    CHECK(offsetof(DropCopyMessage, side) == 31);

    // timestamp_tsc is followed by 24 bytes of padding, because DropCopyMessage
    // is alignas(32) and so cannot start at offset 8.
    CHECK(sizeof(OrderLogEntry) == 64);
    CHECK(offsetof(OrderLogEntry, timestamp_tsc) == 0);
    CHECK(offsetof(OrderLogEntry, msg) == 32);

    // The audit-log header layout is frozen (prep-doc §5): readers depend on
    // these offsets exactly.
    CHECK(sizeof(AuditLogHeader) == 64);
    CHECK(AUDIT_LOG_MAGIC == 0x48465441554401ULL);
    CHECK(offsetof(AuditLogHeader, magic) == 0);
    CHECK(offsetof(AuditLogHeader, version) == 8);
    CHECK(offsetof(AuditLogHeader, entry_size) == 12);
    CHECK(offsetof(AuditLogHeader, write_index) == 16);
}
