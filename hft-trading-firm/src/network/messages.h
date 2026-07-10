#pragma once
#include <cstdint>

enum class Side : uint8_t { BUY, SELL };
enum class MsgType : uint8_t { NEW, CANCEL };
enum class ReportType : uint8_t { FILL, CANCEL, ADD, REJECT };

#pragma pack(push, 1)

// --- OUCH 4.2 Inbound Protocol ---

struct OuchEnterOrder {
    char msg_type;           // 'O'
    char order_token[14];    // Client Order ID (String)
    char side;               // 'B' or 'S'
    uint32_t shares;         // Quantity
    char stock[8];           // e.g. "AAPL    "
    uint32_t price;          // Price (implied 4 decimal places)
    uint32_t time_in_force;  // 99998 = DAY
    char firm[4];            // "MPID"
    char display;            // 'Y'
    char capacity;           // 'P' or 'A'
    char iso_eligibility;    // 'Y' or 'N'
    uint32_t min_quantity;
    char cross_type;
    char customer_type;

    // EXPERIMENT 4: Latency Decomposition
    uint64_t t1_exchange_send;
    uint64_t t2_trading_recv;
    uint64_t t3_trading_enq;
    uint64_t t4_network_deq;
};

struct OuchCancelOrder {
    char msg_type;           // 'X'
    char order_token[14];
    uint32_t shares;
}; // 19 bytes

struct OuchExecutionReport {
    char msg_type;           // 'E'
    char order_token[14];
    uint32_t executed_shares;
    uint32_t execution_price;
    char liquidity_flag;     // 'A' (Added) or 'R' (Removed)
    char match_number[8];
}; // 32 bytes

// --- ITCH 5.0 Outbound Market Data Protocol ---

struct ItchMessage {
    char msg_type;           // 'A', 'E', 'X'
    uint16_t stock_locate;   // Fast Instrument ID
    uint16_t tracking_number;
    uint64_t timestamp;
    uint64_t internal_id;
    uint32_t shares;
    uint64_t price;
    char side;               // 'B' or 'S'
};

#pragma pack(pop)

// Wire-layout guards. These structs are the on-the-wire contract and are
// duplicated in hft_engine/src/protocol/messages.h; both copies carry the same
// asserts so any accidental divergence fails the build instead of silently
// corrupting decoded messages. Do not change without versioning the protocol.
static_assert(sizeof(OuchEnterOrder) == 81, "OuchEnterOrder wire layout changed");
static_assert(sizeof(OuchCancelOrder) == 19, "OuchCancelOrder wire layout changed");
static_assert(sizeof(OuchExecutionReport) == 32, "OuchExecutionReport wire layout changed");
static_assert(sizeof(ItchMessage) == 34, "ItchMessage wire layout changed");
