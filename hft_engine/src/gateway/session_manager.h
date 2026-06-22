#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include "protocol/messages.h"

struct OrderSessionData {
    uint64_t internal_id;
    uint16_t instrument_id;
};

// Maps arbitrary client_order_id to sequential internal_id for the Engine.
class SessionManager {
private:
    static const uint64_t MAX_CLIENT_ORDERS = 50000000; // 50M capacity
    OrderSessionData* client_to_internal;
    uint64_t next_internal_id;

public:
    SessionManager() : next_internal_id(1) {
        client_to_internal = static_cast<OrderSessionData*>(std::malloc(sizeof(OrderSessionData) * MAX_CLIENT_ORDERS));
        if (!client_to_internal) throw std::bad_alloc();
        std::memset(client_to_internal, 0, sizeof(OrderSessionData) * MAX_CLIENT_ORDERS);
    }

    ~SessionManager() {
        std::free(client_to_internal);
    }

    inline uint64_t assign_internal_id(uint64_t client_order_id, uint16_t instrument_id) {
        uint64_t internal_id = next_internal_id++;
        if (client_order_id < MAX_CLIENT_ORDERS) [[likely]] {
            client_to_internal[client_order_id] = {internal_id, instrument_id};
        }
        return internal_id;
    }

    inline OrderSessionData lookup_data(uint64_t client_order_id) const {
        if (client_order_id < MAX_CLIENT_ORDERS) [[likely]] {
            return client_to_internal[client_order_id];
        }
        return {0, 0}; // 0 implies unknown or unregistered
    }
};
