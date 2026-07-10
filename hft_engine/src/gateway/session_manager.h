#pragma once
#include <cstdint>
#include <cstdlib>
#include <atomic>
#include <new>
#include <stdexcept>
#include "protocol/messages.h"

struct OrderSessionData {
    uint64_t internal_id;
    uint16_t instrument_id;
};

// Maps arbitrary client_order_id to its engine handle (the memory-pool slot index,
// which the gateway supplies) plus the instrument it trades, so a later cancel can
// be routed to the right shard and resolved to the resting order.
//
// Thread-safety: multiple SO_REUSEPORT gateway workers share one SessionManager.
// Each map slot is a single atomic<uint64_t> packing internal_id (high 48 bits) and
// instrument_id (low 16 bits), making record/lookup tear-free without a lock;
// internal_id is bounded by the per-shard pool capacity (< 2^48). A slot value of 0
// means "unregistered" — slot index 0 is reserved by MemoryPool as a null handle so
// a real order never packs to 0.
class SessionManager {
private:
    static const uint64_t MAX_CLIENT_ORDERS = 50000000; // 50M capacity
    std::atomic<uint64_t>* client_to_internal; // packed: (internal_id << 16) | instrument_id

public:
    SessionManager() {
        client_to_internal = static_cast<std::atomic<uint64_t>*>(
            std::malloc(sizeof(std::atomic<uint64_t>) * MAX_CLIENT_ORDERS));
        if (!client_to_internal) throw std::bad_alloc();
        // Placement-new each slot initialized to 0 ("unregistered").
        for (uint64_t i = 0; i < MAX_CLIENT_ORDERS; ++i) {
            new (&client_to_internal[i]) std::atomic<uint64_t>(0);
        }
    }

    ~SessionManager() {
        std::free(client_to_internal);
    }

    inline void record_order(uint64_t client_order_id, uint64_t internal_id, uint16_t instrument_id) {
        if (client_order_id < MAX_CLIENT_ORDERS) [[likely]] {
            uint64_t packed = (internal_id << 16) | instrument_id;
            client_to_internal[client_order_id].store(packed, std::memory_order_relaxed);
        }
    }

    inline OrderSessionData lookup_data(uint64_t client_order_id) const {
        if (client_order_id < MAX_CLIENT_ORDERS) [[likely]] {
            uint64_t packed = client_to_internal[client_order_id].load(std::memory_order_relaxed);
            return { packed >> 16, static_cast<uint16_t>(packed & 0xFFFF) };
        }
        return {0, 0}; // 0 implies unknown or unregistered
    }
};
