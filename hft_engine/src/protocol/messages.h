#pragma once
#include <cstdint>

// The single source of truth for every on-the-wire structure in the ecosystem.
// Both the engine and hft-trading-firm include THIS file; there is no second copy.
//
// Bump PROTOCOL_VERSION in the same commit as any change to a packed struct
// below, and update both the static_asserts at the bottom of this file and
// monitoring/wire.py. The stats region (prep-doc §4) publishes this constant so
// Python can assert it is decoding a build it understands.
inline constexpr uint32_t PROTOCOL_VERSION = 1;

enum class Side : uint8_t { BUY, SELL };
enum class MsgType : uint8_t { NEW, CANCEL };
enum class ReportType : uint8_t { FILL, CANCEL, ADD, REJECT };

// --- Canonical symbol encoding ---
//
// The wire carries symbols as "STK" followed by exactly 5 ASCII digits, in the
// 8-byte OuchEnterOrder::stock field: STK00000 .. STK00255. There is one
// encoder and one decoder and they live here, next to the structs they act on,
// so a client and the gateway physically cannot disagree about what a symbol
// means — the situation review A1 catalogued, where four clients used four
// encodings and the documented benchmark matched zero orders.
//
// Instrument ids run [0, MAX_INSTRUMENTS). The cap is 256 because the price
// ladder's bitmaps are per instrument per shard: raising it multiplies book
// memory for no benchmark benefit, since a 256-symbol workload already
// exercises every book path.
inline constexpr uint16_t MAX_INSTRUMENTS = 256;

// Returned by decode_symbol for anything that is not a valid in-range symbol.
// It is deliberately >= MAX_INSTRUMENTS so the risk engine's range check
// rejects it without needing a special case.
inline constexpr uint16_t INVALID_INSTRUMENT = 0xFFFF;

// Decodes an 8-byte wire symbol to an instrument id, or INVALID_INSTRUMENT.
// Every rejection path is explicit: wrong prefix, a non-digit anywhere in the
// numeric field, or an id at/above the cap.
inline uint16_t decode_symbol(const char* stock) {
    if (stock[0] != 'S' || stock[1] != 'T' || stock[2] != 'K') {
        return INVALID_INSTRUMENT;
    }
    uint32_t id = 0;
    for (int i = 3; i < 8; ++i) {
        const char c = stock[i];
        if (c < '0' || c > '9') return INVALID_INSTRUMENT;
        id = id * 10 + static_cast<uint32_t>(c - '0');
    }
    if (id >= MAX_INSTRUMENTS) return INVALID_INSTRUMENT;
    return static_cast<uint16_t>(id);
}

// --- Order tokens ---
//
// OUCH says order_token is an opaque, client-chosen value that the exchange
// echoes back. It identifies an ORDER (it is how a client cancels one); it is
// NOT a client identity — the gateway assigns that from the connection.
//
// This exchange additionally requires the token to be 14 ASCII digits. The old
// decode instead walked the field keeping only the digits, so "ABC00001" and
// "00001XYZ" both collapsed to 1 and silently became the same order (review
// A6). Requiring the full field to be numeric turns that silent collision into
// an explicit rejection; all in-repo clients already zero-pad decimal tokens.
inline constexpr uint64_t INVALID_ORDER_TOKEN = 0xFFFFFFFFFFFFFFFFULL;

inline uint64_t decode_order_token(const char* token) {
    uint64_t value = 0;
    for (int i = 0; i < 14; ++i) {
        const char c = token[i];
        if (c < '0' || c > '9') return INVALID_ORDER_TOKEN;
        value = value * 10 + static_cast<uint64_t>(c - '0');
    }
    return value;
}

// Writes the canonical 8-byte wire symbol for `inst`. Every client uses this
// rather than a hand-rolled literal; that is the whole point of the pair.
inline void encode_symbol(char* stock, uint16_t inst) {
    stock[0] = 'S';
    stock[1] = 'T';
    stock[2] = 'K';
    stock[3] = static_cast<char>('0' + (inst / 10000) % 10);
    stock[4] = static_cast<char>('0' + (inst / 1000) % 10);
    stock[5] = static_cast<char>('0' + (inst / 100) % 10);
    stock[6] = static_cast<char>('0' + (inst / 10) % 10);
    stock[7] = static_cast<char>('0' + inst % 10);
}

#pragma pack(push, 1)

// --- OUCH 4.2 Inbound Protocol ---

struct OuchEnterOrder {
    char msg_type;           // 'O'
    char order_token[14];    // Client Order ID (String)
    char side;               // 'B' or 'S'
    uint32_t shares;         // Quantity
    char stock[8];           // canonical only: "STK00000".."STK00255" (encode_symbol)
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

// Wire-layout guards. These structs are the on-the-wire contract; any accidental
// change to a size fails the build instead of silently corrupting decoded
// messages. Do not change without bumping PROTOCOL_VERSION above.
static_assert(sizeof(OuchEnterOrder) == 81, "OuchEnterOrder wire layout changed");
static_assert(sizeof(OuchCancelOrder) == 19, "OuchCancelOrder wire layout changed");
static_assert(sizeof(OuchExecutionReport) == 32, "OuchExecutionReport wire layout changed");
static_assert(sizeof(ItchMessage) == 34, "ItchMessage wire layout changed");
