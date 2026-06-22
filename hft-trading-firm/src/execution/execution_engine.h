#pragma once
#include <iostream>
#include <array>
#include "../../include/models/NormalizedModels.h"
#include "../../include/network/IExchangeConnector.h"
#include "../risk/risk_manager.h"

// Define max orders we can track to keep it lock-free and flat
constexpr size_t MAX_LIVE_ORDERS = 1000000;

struct TrackedOrder {
    uint16_t instrument_id;
    bool is_buy;
    bool is_active;
};

class ExecutionEngine {
private:
    IExchangeConnector* connector;
    RiskManager* risk_mgr;
    std::array<TrackedOrder, MAX_LIVE_ORDERS> order_tracker;

public:
    ExecutionEngine(IExchangeConnector* conn, RiskManager* risk) 
        : connector(conn), risk_mgr(risk) {
        
        // Initialize tracker array
        for (auto& order : order_tracker) {
            order.is_active = false;
        }

        // Set the execution callback on the connector to route back to us
        if (connector) {
            connector->set_execution_callback([this](uint64_t order_id, uint32_t qty) {
                this->on_execution_report(order_id, qty);
            });
        }
    }

    void on_execution_report(uint64_t order_id, uint32_t filled_qty) {
        if (order_id < MAX_LIVE_ORDERS && order_tracker[order_id].is_active) {
            const auto& track = order_tracker[order_id];
            risk_mgr->on_fill(track.instrument_id, track.is_buy, filled_qty);
            // If fully filled, we would mark is_active = false, but since we don't 
            // know if it's a partial fill without more data, we just pass the fill.
        } else {
            std::cerr << "[ExecutionEngine] Execution received for unknown Order ID: " << order_id << "\n";
        }
    }

    void execute_action(const NormalizedOrderAction& action) {
        // 1. Pre-Trade Risk Check (Now done externally in main.cpp for benchmarking)

        // 2. Track the order
        if (action.internal_order_id < MAX_LIVE_ORDERS) {
            order_tracker[action.internal_order_id] = {
                action.instrument_id,
                action.is_buy,
                true
            };
        }

        // 3. Dispatch to the Exchange Connector
        connector->send_order(action);
    }
};
