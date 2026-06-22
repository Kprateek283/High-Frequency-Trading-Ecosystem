#pragma once
#include <array>
#include <cstdint>
#include <iostream>
#include "../../include/models/NormalizedModels.h"

const uint32_t MAX_PRICE_LEVELS = 100001;
const uint16_t NUM_INSTRUMENTS = 1000;

#include <map>

struct BookSnapshot {
    uint32_t best_bid;
    uint32_t best_ask;
    uint32_t bid_qty;
    uint32_t ask_qty;
};

struct HotBook {
    std::array<uint32_t, MAX_PRICE_LEVELS> bids{0};
    std::array<uint32_t, MAX_PRICE_LEVELS> asks{0};
    
    uint32_t best_bid = 0;
    uint32_t best_ask = MAX_PRICE_LEVELS - 1;

    inline void process_tick(const NormalizedTick& tick) {
        if (tick.price >= MAX_PRICE_LEVELS) [[unlikely]] return;

        if (tick.is_bid) {
            if (!tick.is_trade) { 
                bids[tick.price] += tick.quantity;
                if (tick.price > best_bid) best_bid = tick.price;
            } else { 
                if (bids[tick.price] >= tick.quantity) [[likely]] {
                    bids[tick.price] -= tick.quantity;
                } else {
                    bids[tick.price] = 0;
                }
                
                if (bids[tick.price] == 0 && tick.price == best_bid) {
                    uint32_t p = best_bid;
                    while (p > 0 && bids[p] == 0) p--;
                    best_bid = p;
                }
            }
        } else {
            if (!tick.is_trade) { 
                asks[tick.price] += tick.quantity;
                if (tick.price < best_ask) best_ask = tick.price;
            } else { 
                if (asks[tick.price] >= tick.quantity) [[likely]] {
                    asks[tick.price] -= tick.quantity;
                } else {
                    asks[tick.price] = 0;
                }
                
                if (asks[tick.price] == 0 && tick.price == best_ask) {
                    uint32_t p = best_ask;
                    while (p < MAX_PRICE_LEVELS - 1 && asks[p] == 0) p++;
                    best_ask = p;
                }
            }
        }
    }
    
    inline BookSnapshot get_snapshot() const {
        return {best_bid, best_ask, bids[best_bid], asks[best_ask]};
    }
};

#include <boost/container/flat_map.hpp>

struct ColdBook {
    boost::container::flat_map<uint32_t, uint32_t, std::greater<uint32_t>> bids;
    boost::container::flat_map<uint32_t, uint32_t> asks;

    inline void process_tick(const NormalizedTick& tick) {
        if (tick.is_bid) {
            if (!tick.is_trade) {
                bids[tick.price] += tick.quantity;
            } else {
                auto it = bids.find(tick.price);
                if (it != bids.end()) {
                    if (it->second > tick.quantity) {
                        it->second -= tick.quantity;
                    } else {
                        bids.erase(it);
                    }
                }
            }
        } else {
            if (!tick.is_trade) {
                asks[tick.price] += tick.quantity;
            } else {
                auto it = asks.find(tick.price);
                if (it != asks.end()) {
                    if (it->second > tick.quantity) {
                        it->second -= tick.quantity;
                    } else {
                        asks.erase(it);
                    }
                }
            }
        }
    }

    inline BookSnapshot get_snapshot() const {
        uint32_t bb = bids.empty() ? 0 : bids.begin()->first;
        uint32_t ba = asks.empty() ? MAX_PRICE_LEVELS - 1 : asks.begin()->first;
        uint32_t bq = bids.empty() ? 0 : bids.begin()->second;
        uint32_t aq = asks.empty() ? 0 : asks.begin()->second;
        return {bb, ba, bq, aq};
    }
};

class BookBuilder {
private:
    std::array<HotBook, 64> hot_books;
    std::array<ColdBook, NUM_INSTRUMENTS - 64> cold_books;

public:
    inline void process_tick(const NormalizedTick& tick) {
        if (tick.instrument_id < 64) {
            hot_books[tick.instrument_id].process_tick(tick);
        } else if (tick.instrument_id < NUM_INSTRUMENTS) [[likely]] {
            cold_books[tick.instrument_id - 64].process_tick(tick);
        }
    }

    inline BookSnapshot get_snapshot(uint16_t instrument_id) const {
        if (instrument_id < 64) {
            return hot_books[instrument_id].get_snapshot();
        } else {
            return cold_books[instrument_id - 64].get_snapshot();
        }
    }
};
