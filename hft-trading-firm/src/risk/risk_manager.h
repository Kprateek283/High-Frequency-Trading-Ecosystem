#pragma once
#include "../../include/models/NormalizedModels.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <x86intrin.h>

class RiskManager {
private:
  std::atomic<bool> kill_switch{false};

  // Risk Parameters
  const uint32_t MAX_ORDER_QTY = 1000;
  const int32_t MAX_POSITION = 10000;
  const uint64_t MSG_RATE_LIMIT_PER_SEC = 5000;

  // State. Position is CONFIRMED (ack-driven): it only moves in on_fill (tcp_rx
  // thread). check_order (md thread) only READS it, so there is exactly one home
  // for position and no intent/pre-send double count. Atomic (relaxed) makes the
  // cross-thread access well-defined; these are independent counters, no ordering
  // needed between them.
  std::array<std::atomic<int32_t>, 1000> current_positions{};
  // Realized PnL as running fill cash-flow: sells add proceeds, buys subtract
  // cost. Equals closed PnL whenever the position returns flat. Written on the
  // tcp_rx thread, sampled by the Stage-3 md-thread writer -> atomic.
  // ponytail: cash-flow proxy; switch to per-instrument average-cost if you need
  // realized-vs-unrealized split while holding inventory.
  std::atomic<int64_t> realized_pnl_{0};

  // Rate Limiting State
  uint64_t msg_count_this_second = 0;
  uint64_t current_second = 0;

  // Use RDTSC for fast time approximations (Assumes ~3.5 GHz CPU)
  inline uint64_t get_time_seconds() const {
    unsigned aux;
    return __rdtscp(&aux) / 3500000000ULL;
  }

public:
  RiskManager() { kill_switch = false; }

  inline void trigger_kill_switch() {
    kill_switch.store(true, std::memory_order_release);
    std::cout << "[RISK] KILL SWITCH ENGAGED! ALL OUTBOUND FLOW BLOCKED.\n";
  }

  // Called when the firm gets a CONFIRMED execution report from the exchange.
  inline void on_fill(uint16_t instrument_id, bool is_buy, uint32_t qty,
                      uint32_t price) {
    int64_t notional = static_cast<int64_t>(price) * qty;
    if (is_buy) {
      current_positions[instrument_id].fetch_add((int32_t)qty, std::memory_order_relaxed);
      realized_pnl_.fetch_sub(notional, std::memory_order_relaxed);
    } else {
      current_positions[instrument_id].fetch_sub((int32_t)qty, std::memory_order_relaxed);
      realized_pnl_.fetch_add(notional, std::memory_order_relaxed);
    }
  }

  // Read accessors for monitoring (Stage 3 seqlock writer samples these).
  inline int32_t position(uint16_t instrument_id) const {
    return current_positions[instrument_id].load(std::memory_order_relaxed);
  }
  inline int64_t realized_pnl() const {
    return realized_pnl_.load(std::memory_order_relaxed);
  }
  inline bool kill_switch_engaged() const {
    return kill_switch.load(std::memory_order_acquire);
  }

  // O(1) Pre-Trade Risk Check. Returns true if the order is SAFE to send.
  inline bool check_order(const NormalizedOrderAction &action) {
    if (kill_switch.load(std::memory_order_acquire)) [[unlikely]] {
      return false;
    }

    if (action.is_cancel) {
      return true; // Cancels are always risk-free
    }

    // 1. Fat Finger Check
    if (action.quantity > MAX_ORDER_QTY) [[unlikely]] {
      std::cout << "[RISK] Fat Finger Blocked! Qty: " << action.quantity
                << "\n";
      return false;
    }

    // 2. Inventory Check (Prevent accumulating too much risk)
    int32_t current_pos = current_positions[action.instrument_id].load(std::memory_order_relaxed);
    if (action.is_buy) {
      if (current_pos + (int32_t)action.quantity > MAX_POSITION) [[unlikely]] {
        return false;
      }
    } else {
      if (current_pos - (int32_t)action.quantity < -MAX_POSITION) [[unlikely]] {
        return false;
      }
    }

    // 3. Message Rate Limit (Token Bucket / Rolling Counter)
    uint64_t now_sec = get_time_seconds();
    if (now_sec != current_second) {
      current_second = now_sec;
      msg_count_this_second = 0;
    }

    if (++msg_count_this_second > MSG_RATE_LIMIT_PER_SEC) [[unlikely]] {
      return false;
    }

    return true;
  }
};
