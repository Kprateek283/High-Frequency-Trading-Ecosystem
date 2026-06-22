#pragma once
#include <cstdint>
#include <atomic>
#include "protocol/messages.h"

// Internal engine Order struct with intrusive pointers for the OrderBook
struct alignas(64) Order {
    uint64_t internal_id;      // Sequential ID assigned by SessionManager
    uint64_t client_order_id;  // Needed for drop-copy/reporting
    uint64_t price;
    uint32_t quantity;
    uint16_t instrument_id;
    Side side;

    Order* next = nullptr;
    Order* prev = nullptr;

    Order() = default;

    Order(uint64_t int_id, uint64_t client_id, uint64_t p, uint32_t q, uint16_t inst, Side s) 
        : internal_id(int_id), client_order_id(client_id), price(p), quantity(q), instrument_id(inst), side(s), next(nullptr), prev(nullptr) {}
};

// Padded/Aligned to 32 bytes to ensure it cleanly fits in cache lines in LockFreeQueue
struct alignas(32) EngineTask {
    MsgType type;
    union {
        Order* order;          // Used for MsgType::NEW
        uint64_t internal_id;  // Used for MsgType::CANCEL
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

// Global statistics for monitoring health without slowing down the engine
struct EngineStats {
    std::atomic<uint64_t> dropped_reports{0};
};

extern EngineStats g_stats;
