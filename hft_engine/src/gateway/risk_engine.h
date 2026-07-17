#pragma once
#include "protocol/messages.h"

class RiskEngine {
private:
    uint32_t max_order_qty;
    uint64_t max_price;
    uint64_t max_notional;

public:
    RiskEngine(uint32_t max_qty = 1000000, uint64_t max_p = 10000000, uint64_t max_n = 10000000000ULL)
        : max_order_qty(max_qty), max_price(max_p), max_notional(max_n) {}

    // Inline for zero-overhead hot path calls
    inline bool check_pre_trade(const OuchEnterOrder& req, uint16_t mapped_inst) const {
        if (req.shares == 0 || req.shares > max_order_qty) return false;
        if (req.price == 0 || req.price > max_price) return false;
        if (static_cast<uint64_t>(req.price) * req.shares > max_notional) return false;
        // One definition of instrument validity, from the protocol header. This
        // also rejects INVALID_INSTRUMENT, which is deliberately out of range.
        if (mapped_inst >= MAX_INSTRUMENTS) return false;
        return true;
    }
};
