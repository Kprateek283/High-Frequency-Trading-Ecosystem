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
    uint32_t client_id;     // owning connection, assigned server-side
};

// Maps an order token to its engine handle (the memory-pool slot index, which the
// gateway supplies), the instrument it trades, and the identity of the connection
// that sent it — so a later cancel can be routed to the right shard, resolved to
// the resting order, and checked against the client that actually owns it.
//
// Thread-safety: multiple SO_REUSEPORT gateway workers share one SessionManager.
// Each map slot is a single atomic<uint64_t>, so record/lookup are tear-free
// without a lock. A slot value of 0 means "unregistered" — slot index 0 is
// reserved by MemoryPool as a null handle, so a real order never packs to 0.
class SessionManager {
public:
    // The token space this map can address. The gateway rejects any token at or
    // above this rather than silently failing to register it.
    static constexpr uint64_t MAX_CLIENT_ORDERS = 50000000; // 50M capacity

    // Slot bit layout. The three fields are far smaller than the 48 bits the
    // old (internal_id << 16 | instrument_id) packing spent on internal_id
    // alone, which is what leaves room for client_id here.
    //   bits  0-7   instrument_id  (< MAX_INSTRUMENTS == 256)
    //   bits  8-27  internal_id    (pool slot index, >= 1 for a live order)
    //   bits 32-63  client_id      (server-assigned, per connection)
    static constexpr int INST_BITS     = 8;
    static constexpr int ID_SHIFT      = 8;
    static constexpr int ID_BITS       = 20;
    static constexpr int CLIENT_SHIFT  = 32;
    static constexpr uint64_t MAX_INTERNAL_ID = (1ULL << ID_BITS);

    static_assert(MAX_INSTRUMENTS <= (1u << INST_BITS),
                  "instrument_id no longer fits in its packed field");

private:
    std::atomic<uint64_t>* client_to_internal;

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

    inline void record_order(uint64_t client_order_id, uint64_t internal_id,
                             uint16_t instrument_id, uint32_t client_id) {
        if (client_order_id < MAX_CLIENT_ORDERS) [[likely]] {
            uint64_t packed = (static_cast<uint64_t>(client_id) << CLIENT_SHIFT)
                            | (internal_id << ID_SHIFT)
                            | instrument_id;
            client_to_internal[client_order_id].store(packed, std::memory_order_relaxed);
        }
    }

    inline OrderSessionData lookup_data(uint64_t client_order_id) const {
        if (client_order_id < MAX_CLIENT_ORDERS) [[likely]] {
            uint64_t packed = client_to_internal[client_order_id].load(std::memory_order_relaxed);
            return { (packed >> ID_SHIFT) & (MAX_INTERNAL_ID - 1),
                     static_cast<uint16_t>(packed & ((1u << INST_BITS) - 1)),
                     static_cast<uint32_t>(packed >> CLIENT_SHIFT) };
        }
        return {0, 0, 0}; // internal_id 0 implies unknown or unregistered
    }
};
