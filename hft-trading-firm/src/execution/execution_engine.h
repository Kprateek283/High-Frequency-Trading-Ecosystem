#pragma once
#include <iostream>
#include <array>
#include <atomic>
#include "../../include/models/NormalizedModels.h"
#include "../../include/network/IExchangeConnector.h"
#include "../risk/risk_manager.h"

// Define max orders we can track to keep it lock-free and flat
constexpr size_t MAX_LIVE_ORDERS = 1000000;

struct TrackedOrder {
    uint16_t instrument_id;
    bool is_buy;
    uint32_t remaining_qty;   // shares still open; 0 => fully filled
    // Publish/consume flag across the md thread (writes the slot in
    // execute_action) and the tcp_rx thread (reads it in on_execution_report).
    // The two accesses are separated by the exchange round-trip, so there is no
    // concurrent same-slot write; release/acquire just gives the visibility edge
    // that makes the plain-field writes above observable when is_active reads true.
    std::atomic<bool> is_active{false};
};

class ExecutionEngine {
private:
    IExchangeConnector* connector;
    RiskManager* risk_mgr;
    std::array<TrackedOrder, MAX_LIVE_ORDERS> order_tracker;

    // Ack-channel counters. Touched by the market-data thread (send) and the
    // tcp_rx thread (fills), so atomic. in_flight = live orders sent but not yet
    // fully filled; a partial fill leaves the order in flight.
    std::atomic<uint64_t> orders_acked_{0};
    std::atomic<uint64_t> in_flight_{0};

public:
    uint64_t orders_acked() const { return orders_acked_.load(std::memory_order_relaxed); }
    uint64_t in_flight() const { return in_flight_.load(std::memory_order_relaxed); }

    ExecutionEngine(IExchangeConnector* conn, RiskManager* risk)
        : connector(conn), risk_mgr(risk) {
        // order_tracker slots default-construct with is_active = false.

        // Set the execution callback on the connector to route back to us
        if (connector) {
            connector->set_execution_callback([this](uint64_t order_id, uint32_t qty, uint32_t price) {
                this->on_execution_report(order_id, qty, price);
            });
        }
    }

    // Confirmed fill from the exchange (ack channel). order_id is the firm's own
    // internal_order_id (the connector already stripped TOKEN_BASE from the token).
    void on_execution_report(uint64_t order_id, uint32_t filled_qty, uint32_t price) {
        if (order_id < MAX_LIVE_ORDERS &&
            order_tracker[order_id].is_active.load(std::memory_order_acquire)) {
            auto& track = order_tracker[order_id];
            // Confirmed position + realized PnL live in RiskManager (ack-driven).
            risk_mgr->on_fill(track.instrument_id, track.is_buy, filled_qty, price);
            orders_acked_.fetch_add(1, std::memory_order_relaxed);

            // Partial fill -> reduce remaining, order stays in flight.
            // Full fill -> close the order and drain it from in_flight.
            if (filled_qty >= track.remaining_qty) {
                track.remaining_qty = 0;
                track.is_active.store(false, std::memory_order_relaxed);
                in_flight_.fetch_sub(1, std::memory_order_relaxed);
            } else {
                track.remaining_qty -= filled_qty;
            }
        } else {
            std::cerr << "[ExecutionEngine] Execution received for unknown Order ID: " << order_id << "\n";
        }
    }

    void execute_action(const NormalizedOrderAction& action) {
        // 1. Pre-Trade Risk Check (Now done externally in main.cpp for benchmarking)

        // 2. Track the order (cancels reference an existing order; don't track them)
        if (!action.is_cancel && action.internal_order_id < MAX_LIVE_ORDERS) {
            auto& slot = order_tracker[action.internal_order_id];
            // Write the plain fields FIRST, then publish with a release store so
            // the tcp_rx thread sees them once it acquire-loads is_active.
            slot.instrument_id = action.instrument_id;
            slot.is_buy = action.is_buy;
            slot.remaining_qty = action.quantity;
            slot.is_active.store(true, std::memory_order_release);
            in_flight_.fetch_add(1, std::memory_order_relaxed);
        }

        // 3. Dispatch to the Exchange Connector
        connector->send_order(action);
    }
};
