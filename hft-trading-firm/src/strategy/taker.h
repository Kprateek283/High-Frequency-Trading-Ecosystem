#pragma once
#include <cstdint>
#include <vector>
#include "istrategy.h"
#include "../signal/signal_engine.h"
#include "../../include/models/NormalizedModels.h"

// Aggressive taker (multi-firm-plan Part B): when the order-book imbalance is
// strong enough, cross the touch — lift the ask on bid pressure, hit the bid on
// ask pressure. This is the counterparty to the passive maker: a marketable
// order that actually removes resting liquidity (guaranteed fills when a maker
// is quoting the other side).
class TakerStrategy : public IStrategy {
private:
    uint64_t local_order_counter = 1;
    const float THRESHOLD;      // |imbalance| beyond this fires a cross
    const uint32_t SIZE;

public:
    explicit TakerStrategy(float threshold = 0.6f, uint32_t size = 100)
        : THRESHOLD(threshold), SIZE(size) {}

    void on_tick(const NormalizedTick& tick, const AlphaSignals& sig,
                 std::vector<NormalizedOrderAction>& outbound_actions) override {
        if (!sig.valid) return;

        // AlphaSignals carries mid + spread, not the raw touch. Reconstruct it:
        // best_bid = mid - spread/2, best_ask = mid + spread/2 (exact for integer
        // prices; +0.5 guards float rounding).
        if (sig.imbalance > THRESHOLD) {
            // Bid pressure -> price likely rising -> BUY, crossing the ask.
            uint32_t best_ask =
                static_cast<uint32_t>(sig.mid_price + sig.spread / 2.0f + 0.5f);
            outbound_actions.push_back({local_order_counter++, tick.instrument_id,
                                        best_ask, SIZE, true, false,
                                        tick.t1_exchange_send, tick.t2_trading_recv, 0});
        } else if (sig.imbalance < -THRESHOLD) {
            // Ask pressure -> price likely falling -> SELL, crossing the bid.
            uint32_t best_bid =
                static_cast<uint32_t>(sig.mid_price - sig.spread / 2.0f + 0.5f);
            outbound_actions.push_back({local_order_counter++, tick.instrument_id,
                                        best_bid, SIZE, false, false,
                                        tick.t1_exchange_send, tick.t2_trading_recv, 0});
        }
    }
};
