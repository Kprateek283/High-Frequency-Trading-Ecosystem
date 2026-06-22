#pragma once
#include <functional>
#include "models/NormalizedModels.h"

class IExchangeConnector {
public:
    virtual ~IExchangeConnector() = default;

    // Connects to the venue (TCP/Websockets/REST)
    virtual bool initialize() = 0;

    // Starts pumping normalized ticks into our Brain
    using TickCallback = std::function<void(const NormalizedTick&)>;
    virtual void start_market_data(TickCallback on_tick) = 0;

    // Sets the callback for when our orders are filled (passes the internal_order_id and filled quantity)
    using ExecutionCallback = std::function<void(uint64_t internal_order_id, uint32_t qty)>;
    virtual void set_execution_callback(ExecutionCallback on_fill) = 0;

    // Transmits our intent to the venue
    virtual void send_order(const NormalizedOrderAction& action) = 0;
};
