#pragma once
#include <cstdint>
#include <vector>
#include "../signal/signal_engine.h"
#include "../../include/models/NormalizedModels.h"

class MarketMakerStrategy {
private:
    uint32_t current_bid_price[1000] = {0};
    uint32_t current_ask_price[1000] = {0};
    uint64_t local_order_counter = 1;

    // Strategy Parameters
    const uint32_t SPREAD_MARGIN = 2; // Capture 4 ticks total (2 on each side)
    const float IMBALANCE_THRESHOLD = 0.6f;

public:
    // Takes the latest AlphaSignals and generates necessary OrderActions
    inline void on_tick(const NormalizedTick& tick, const AlphaSignals& sig, std::vector<NormalizedOrderAction>& outbound_actions) {
        if (!sig.valid) return;

        uint16_t instrument_id = tick.instrument_id;
        
        // Calculate our "Fair Value"
        float fair_value = sig.micro_price;

        // Skew quotes based on Order Book Imbalance (Defensive Quoting)
        if (sig.imbalance > IMBALANCE_THRESHOLD) {
            // Heavy Bid pressure. Price will likely go up.
            // Skew quotes UP to avoid getting run over on the Ask side.
            fair_value += 1.0f; 
        } else if (sig.imbalance < -IMBALANCE_THRESHOLD) {
            // Heavy Ask pressure. Price will likely go down.
            fair_value -= 1.0f;
        }

        uint32_t desired_bid = static_cast<uint32_t>(fair_value) - SPREAD_MARGIN;
        uint32_t desired_ask = static_cast<uint32_t>(fair_value) + SPREAD_MARGIN + 1; // +1 to handle rounding

        // Check if we need to update our Bid
        if (desired_bid != current_bid_price[instrument_id]) {
            if (current_bid_price[instrument_id] != 0) {
                // Cancel old bid
                outbound_actions.push_back({local_order_counter++, instrument_id, current_bid_price[instrument_id], 100, true, true, tick.t1_exchange_send, tick.t2_trading_recv, 0});
            }
            // Send new bid
            outbound_actions.push_back({local_order_counter++, instrument_id, desired_bid, 100, true, false, tick.t1_exchange_send, tick.t2_trading_recv, 0});
            current_bid_price[instrument_id] = desired_bid;
        }

        // Check if we need to update our Ask
        if (desired_ask != current_ask_price[instrument_id]) {
            if (current_ask_price[instrument_id] != 0) {
                // Cancel old ask
                outbound_actions.push_back({local_order_counter++, instrument_id, current_ask_price[instrument_id], 100, false, true, tick.t1_exchange_send, tick.t2_trading_recv, 0});
            }
            // Send new ask
            outbound_actions.push_back({local_order_counter++, instrument_id, desired_ask, 100, false, false, tick.t1_exchange_send, tick.t2_trading_recv, 0});
            current_ask_price[instrument_id] = desired_ask;
        }
    }
};
