#pragma once
#include <cstdint>
#include <functional>
#include "models/NormalizedModels.h"

// Connector-side cumulative counters the firm monitor samples (firm-monitoring-plan).
struct ConnectorStats {
    uint64_t orders_sent = 0;
    uint64_t serialize_cycles = 0;
    uint64_t send_cycles = 0;
    uint64_t enqueue_cycles = 0;
    uint64_t dequeue_cycles = 0;
};

class IExchangeConnector {
public:
    virtual ~IExchangeConnector() = default;

    // Connects to the venue (TCP/Websockets/REST)
    virtual bool initialize() = 0;

    // Snapshot of the connector's cumulative counters for monitoring.
    virtual ConnectorStats stats() const = 0;

    // Starts pumping normalized ticks into our Brain
    using TickCallback = std::function<void(const NormalizedTick&)>;
    virtual void start_market_data(TickCallback on_tick) = 0;

    // Sets the callback for when our orders are filled (internal_order_id, filled quantity, execution price)
    using ExecutionCallback = std::function<void(uint64_t internal_order_id, uint32_t qty, uint32_t price)>;
    virtual void set_execution_callback(ExecutionCallback on_fill) = 0;

    // Transmits our intent to the venue
    virtual void send_order(const NormalizedOrderAction& action) = 0;
};
