#pragma once
#include <cstdint>
#include "../data/book_builder.h"

struct AlphaSignals {
    float mid_price;
    float micro_price;
    float imbalance;   // Range: [-1.0, 1.0]
    uint32_t spread;
    bool valid;
};

class SignalEngine {
public:
    inline AlphaSignals compute_signals(const BookSnapshot& snap) const {
        AlphaSignals sig;
        sig.valid = false;

        uint32_t best_bid = snap.best_bid;
        uint32_t best_ask = snap.best_ask;

        // MAX_PRICE_LEVELS - 1 is 100000
        if (best_bid == 0 || best_ask == 100000) {
            return sig; // Not enough data to form a market
        }

        uint32_t bid_qty = snap.bid_qty;
        uint32_t ask_qty = snap.ask_qty;

        if (bid_qty == 0 && ask_qty == 0) {
            return sig;
        }

        sig.spread = best_ask - best_bid;
        sig.mid_price = (best_bid + best_ask) / 2.0f;

        // Order Book Imbalance (OBI)
        // High positive = heavy bid pressure (bullish)
        // High negative = heavy ask pressure (bearish)
        float total_qty = static_cast<float>(bid_qty + ask_qty);
        sig.imbalance = static_cast<float>(bid_qty - ask_qty) / total_qty;

        // Volume-weighted Micro-price
        // Shifts the mid_price closer to the side with more liquidity
        sig.micro_price = (best_bid * ask_qty + best_ask * bid_qty) / total_qty;
        
        sig.valid = true;
        return sig;
    }
};
