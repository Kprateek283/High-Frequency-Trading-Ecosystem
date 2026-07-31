#pragma once
#include <cstdint>
#include <atomic>
#include "protocol/messages.h"

// Internal engine Order struct with intrusive pointers for the OrderBook.
//
// Two distinct identifiers, deliberately not the same thing:
//   client_order_id  the client's own order token (decoded from order_token).
//                    Identifies an ORDER; it is what a cancel names and what
//                    the drop copy echoes back.
//   client_id        the connection that sent it, assigned server-side by the
//                    gateway. Identifies a CLIENT. Self-trade prevention
//                    compares this, never the token.
struct alignas(64) Order {
    uint64_t internal_id;      // pool slot index; also the ITCH order reference
    uint64_t client_order_id;  // client's order token, for drop-copy/reporting
    uint64_t price;
    uint32_t quantity;
    uint32_t client_id;        // owning connection (server-assigned identity)
    uint16_t instrument_id;
    uint16_t worker_id;        // gateway worker owning this order's client fd;
                               // routes the private ack back (private-ack-plan §1)
    Side side;

    Order* next = nullptr;
    Order* prev = nullptr;

    Order() = default;

    Order(uint64_t int_id, uint64_t client_order_tok, uint32_t owner, uint64_t p,
          uint32_t q, uint16_t inst, Side s, uint16_t wid = 0)
        : internal_id(int_id), client_order_id(client_order_tok), price(p),
          quantity(q), client_id(owner), instrument_id(inst), worker_id(wid),
          side(s), next(nullptr), prev(nullptr) {}
};

// Padded/Aligned to 32 bytes to ensure it cleanly fits in cache lines in LockFreeQueue
struct alignas(32) EngineTask {
    MsgType type;
    struct CancelData {
        uint64_t internal_id;      // pool slot handle of the resting order
        uint64_t client_order_id;  // validated against the slot's current owner
    };
    union {
        Order* order;          // Used for MsgType::NEW
        CancelData cancel;     // Used for MsgType::CANCEL
    };
    uint64_t ingress_tsc;
};

enum class OrderState : uint8_t {
    NEW = 0,
    PARTIAL_FILL,
    FILLED,
    CANCELED,
    REJECTED
};

struct alignas(32) DropCopyMessage {
    uint64_t client_order_id;
    uint64_t internal_id;
    uint64_t price;
    uint32_t quantity;
    uint16_t instrument_id;
    OrderState state;
    Side side;
};

// Queue-internal egress envelope (private-ack §2/§4): the 32-byte wire report the
// gateway writes to the socket, plus the client_id the owning worker needs to look
// up the fd. client_id is NEVER sent — it stays inside the ack queue.
struct OutboundAck {
    OuchExecutionReport report;
    uint32_t client_id;
};

// Global statistics for monitoring health without slowing down the engine
struct EngineStats {
    std::atomic<uint64_t> dropped_reports{0};
    std::atomic<uint64_t> dropped_drop_copies{0};
    alignas(64) std::atomic<uint64_t> engine_orders_in[8];
    alignas(64) std::atomic<uint64_t> engine_polls_idle[8];
    alignas(64) std::atomic<uint64_t> engine_polls_active[8];
};

extern EngineStats g_stats;
