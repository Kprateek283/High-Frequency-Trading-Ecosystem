#pragma once
#include <cstdint>

struct NormalizedTick {
    uint16_t instrument_id;
    uint64_t price;
    uint32_t quantity;
    bool is_bid;
    bool is_trade; // True if it was an execution, False if just an orderbook update

    // Latency Decomposition
    uint64_t t1_exchange_send;
    uint64_t t2_trading_recv;
};

struct NormalizedOrderAction {
    uint64_t internal_order_id;
    uint16_t instrument_id;
    uint64_t price;
    uint32_t quantity;
    bool is_buy;
    bool is_cancel;

    // Latency Decomposition
    uint64_t t1_exchange_send;
    uint64_t t2_trading_recv;
    uint64_t t3_trading_enq;
};
