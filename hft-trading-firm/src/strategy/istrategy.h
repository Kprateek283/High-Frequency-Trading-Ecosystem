#pragma once
#include <vector>
#include "../signal/signal_engine.h"
#include "../../include/models/NormalizedModels.h"

// Minimal strategy interface (multi-firm-plan Part B). Justified by two impls:
// MarketMakerStrategy (passive) and TakerStrategy (aggressive). main.cpp holds
// the chosen one via IStrategy* and dispatches per tick.
class IStrategy {
public:
    virtual ~IStrategy() = default;
    virtual void on_tick(const NormalizedTick& tick, const AlphaSignals& sig,
                         std::vector<NormalizedOrderAction>& outbound_actions) = 0;
};
