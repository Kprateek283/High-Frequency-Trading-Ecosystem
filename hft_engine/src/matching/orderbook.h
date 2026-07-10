#pragma once
#include <vector>
#include <cstring>
#include <algorithm>
#include <atomic>
#include "matching/order.h"
#include "core/memory_pool.h"
#include "core/lock_free_queue.h"

const uint32_t MAX_PRICE = 100001;
const uint32_t MAX_ORDERS_LOOKUP = 20000001; 

class OrderBook {
private:
    struct Limit {
        Order* head = nullptr;
        Order* tail = nullptr;

        inline void add(Order* o) {
            if (!tail) {
                head = tail = o;
                o->next = o->prev = nullptr;
            } else {
                tail->next = o;
                o->prev = tail;
                o->next = nullptr;
                tail = o;
            }
        }

        inline void remove(Order* o) {
            if (o->prev) o->prev->next = o->next;
            if (o->next) o->next->prev = o->prev;
            if (o == head) head = o->next;
            if (o == tail) tail = o->prev;
            o->next = o->prev = nullptr;
        }
    };

    Limit bids[MAX_PRICE];
    Limit asks[MAX_PRICE];
    Order** orders_by_id;

    uint64_t bids_bitmap[MAX_PRICE / 64 + 1];
    uint64_t asks_bitmap[MAX_PRICE / 64 + 1];

    // L2 Summary Bitmaps: 1 bit per 64-bit word of L1
    uint64_t bids_l2[MAX_PRICE / 4096 + 1];
    uint64_t asks_l2[MAX_PRICE / 4096 + 1];

    // FIX: BBO Caching
    uint32_t cached_best_bid = 0;
    uint32_t cached_best_ask = MAX_PRICE - 1;

    inline void set_bit(uint64_t* bitmap, uint64_t* l2, uint32_t price) {
        uint32_t word_idx = price >> 6;
        bitmap[word_idx] |= (1ULL << (price & 63));
        l2[word_idx >> 6] |= (1ULL << (word_idx & 63));
    }

    inline void clear_bit(uint64_t* bitmap, uint64_t* l2, uint32_t price) {
        uint32_t word_idx = price >> 6;
        bitmap[word_idx] &= ~(1ULL << (price & 63));
        if (bitmap[word_idx] == 0) {
            l2[word_idx >> 6] &= ~(1ULL << (word_idx & 63));
        }
    }

    // Finds the lowest set ask price >= start_price, or -1 if none.
    inline int32_t find_next_ask(uint32_t start_price) {
        const uint32_t num_l2 = MAX_PRICE / 4096 + 1;
        uint32_t word_idx = start_price >> 6;

        // 1. Remainder of the starting L1 word (bits at/above start_price).
        uint64_t word = asks_bitmap[word_idx] & (~0ULL << (start_price & 63));
        if (word) return (word_idx << 6) + __builtin_ctzll(word);

        // 2. Following L1 words within the current L2 group. Mask keeps bits
        //    strictly above (word_idx & 63); guarded against shift-by-64 UB and
        //    against wrongly re-selecting the current word at the group boundary.
        uint32_t l2_word_idx = word_idx >> 6;
        uint32_t bit_in_l2 = word_idx & 63;
        uint64_t l2_mask = (bit_in_l2 == 63) ? 0ULL : (~0ULL << (bit_in_l2 + 1));
        uint64_t l2_word = asks_l2[l2_word_idx] & l2_mask;
        if (l2_word) {
            uint32_t next_word_idx = (l2_word_idx << 6) + __builtin_ctzll(l2_word);
            return (next_word_idx << 6) + __builtin_ctzll(asks_bitmap[next_word_idx]);
        }

        // 3. Following L2 groups.
        for (uint32_t i = l2_word_idx + 1; i < num_l2; ++i) {
            if (asks_l2[i]) {
                uint32_t next_word_idx = (i << 6) + __builtin_ctzll(asks_l2[i]);
                return (next_word_idx << 6) + __builtin_ctzll(asks_bitmap[next_word_idx]);
            }
        }
        return -1;
    }

    // Finds the highest set bid price <= start_price, or -1 if none.
    inline int32_t find_next_bid(uint32_t start_price) {
        int32_t word_idx = (int32_t)(start_price >> 6);

        // 1. Remainder of the starting L1 word (bits at/below start_price).
        uint64_t word = bids_bitmap[word_idx] & (~0ULL >> (63 - (start_price & 63)));
        if (word) return (word_idx << 6) + (63 - __builtin_clzll(word));

        // 2. Preceding L1 words within the current L2 group. Mask keeps bits
        //    strictly below (word_idx & 63); guarded against shift-by-64 UB and
        //    against wrongly re-selecting the current word at the group boundary.
        int32_t l2_word_idx = word_idx >> 6;
        uint32_t bit_in_l2 = (uint32_t)word_idx & 63;
        uint64_t l2_mask = (bit_in_l2 == 0) ? 0ULL : (~0ULL >> (64 - bit_in_l2));
        uint64_t l2_word = bids_l2[l2_word_idx] & l2_mask;
        if (l2_word) {
            uint32_t next_word_idx = (l2_word_idx << 6) + (63 - __builtin_clzll(l2_word));
            return (next_word_idx << 6) + (63 - __builtin_clzll(bids_bitmap[next_word_idx]));
        }

        // 3. Preceding L2 groups.
        for (int32_t i = l2_word_idx - 1; i >= 0; --i) {
            if (bids_l2[i]) {
                uint32_t next_word_idx = (i << 6) + (63 - __builtin_clzll(bids_l2[i]));
                return (next_word_idx << 6) + (63 - __builtin_clzll(bids_bitmap[next_word_idx]));
            }
        }
        return -1;
    }

    MemoryPool<Order>* pool = nullptr;
    LockFreeQueue<ItchMessage, 1048576>* mkt_data_queue = nullptr;
    LockFreeQueue<DropCopyMessage, 1048576>* drop_copy_queue = nullptr;

    inline void broadcast(char type, uint64_t internal_id, uint64_t price, uint32_t qty, uint16_t inst, char side) {
        ItchMessage report = {type, inst, 0, __builtin_ia32_rdtsc(), internal_id, qty, price, side};
        if (!mkt_data_queue->push(report)) {
            g_stats.dropped_reports.fetch_add(1, std::memory_order_relaxed);
        }
    }

    inline void send_drop_copy(uint64_t client_id, uint64_t internal_id, uint64_t price, uint32_t qty, uint16_t inst, Side side, OrderState state) {
        DropCopyMessage msg = {client_id, internal_id, price, qty, inst, state, side};
        // Don't spin, drop copy queue is huge and read asynchronously.
        drop_copy_queue->push(msg);
    }

public:
    OrderBook() {
        std::memset(static_cast<void*>(bids), 0, sizeof(bids));
        std::memset(static_cast<void*>(asks), 0, sizeof(asks));
        std::memset(bids_bitmap, 0, sizeof(bids_bitmap));
        std::memset(asks_bitmap, 0, sizeof(asks_bitmap));
        std::memset(bids_l2, 0, sizeof(bids_l2));
        std::memset(asks_l2, 0, sizeof(asks_l2));
    }

    void init(MemoryPool<Order>* p, LockFreeQueue<ItchMessage, 1048576>* q, LockFreeQueue<DropCopyMessage, 1048576>* dcq, Order** arr) {
        pool = p;
        mkt_data_queue = q;
        drop_copy_queue = dcq;
        orders_by_id = arr;
    }

    ~OrderBook() {
    }

    void cancel_order(uint64_t id) {
        if (id >= MAX_ORDERS_LOOKUP) [[unlikely]] return;
        Order* o = orders_by_id[id];
        if (!o) [[unlikely]] return;

        broadcast('X', o->internal_id, o->price, o->quantity, o->instrument_id, o->side == Side::BUY ? 'B' : 'S');
        send_drop_copy(o->client_order_id, o->internal_id, o->price, o->quantity, o->instrument_id, o->side, OrderState::CANCELED);

        uint32_t p = o->price;
        if (o->side == Side::BUY) {
            bids[p].remove(o);
            if (!bids[p].head) {
                clear_bit(bids_bitmap, bids_l2, p);
                if (p == cached_best_bid) {
                    int32_t next = find_next_bid(p);
                    cached_best_bid = (next == -1) ? 0 : next;
                }
            }
        } else {
            asks[p].remove(o);
            if (!asks[p].head) {
                clear_bit(asks_bitmap, asks_l2, p);
                if (p == cached_best_ask) {
                    int32_t next = find_next_ask(p);
                    cached_best_ask = (next == -1) ? MAX_PRICE - 1 : next;
                }
            }
        }

        orders_by_id[id] = nullptr;
        pool->deallocate(o);
    }

    void match_order(Order* new_order) {
        if (new_order->internal_id >= MAX_ORDERS_LOOKUP || new_order->price >= MAX_PRICE) [[unlikely]] {
            pool->deallocate(new_order);
            return;
        }
        uint32_t price = static_cast<uint32_t>(new_order->price);

        if (new_order->side == Side::BUY) {
            int32_t p = cached_best_ask;
            while (p != -1 && p <= (int32_t)price && new_order->quantity > 0) {
                Limit& level = asks[p];
                while (new_order->quantity > 0 && level.head) {
                    Order* resting = level.head;
                    if (resting->next) [[likely]] {
                        __builtin_prefetch(resting->next, 0, 3);
                    }
                    uint32_t match_qty = std::min(new_order->quantity, resting->quantity);
                    broadcast('E', resting->internal_id, resting->price, match_qty, resting->instrument_id, resting->side == Side::BUY ? 'B' : 'S');
                    broadcast('E', new_order->internal_id, resting->price, match_qty, new_order->instrument_id, new_order->side == Side::BUY ? 'B' : 'S');
                    
                    send_drop_copy(new_order->client_order_id, new_order->internal_id, resting->price, match_qty, new_order->instrument_id, new_order->side, 
                                   new_order->quantity > resting->quantity ? OrderState::PARTIAL_FILL : OrderState::FILLED);
                    send_drop_copy(resting->client_order_id, resting->internal_id, resting->price, match_qty, resting->instrument_id, resting->side, 
                                   resting->quantity > new_order->quantity ? OrderState::PARTIAL_FILL : OrderState::FILLED);

                    if (new_order->quantity >= resting->quantity) [[likely]] {
                        new_order->quantity -= resting->quantity;
                        orders_by_id[resting->internal_id] = nullptr;
                        level.remove(resting);
                        pool->deallocate(resting);
                    } else {
                        resting->quantity -= new_order->quantity;
                        new_order->quantity = 0;
                    }
                }
                if (!level.head) [[likely]] {
                    clear_bit(asks_bitmap, asks_l2, p);
                    int32_t next = find_next_ask(p + 1);
                    if (next == -1) {
                        p = -1;
                    } else {
                        cached_best_ask = next;
                        p = next;
                    }
                } else break;
            }
            if (new_order->quantity > 0) [[likely]] {
                bool is_new_level = !bids[price].head;
                bids[price].add(new_order);
                if (is_new_level) [[unlikely]] {
                    set_bit(bids_bitmap, bids_l2, price);
                }
                orders_by_id[new_order->internal_id] = new_order;
                broadcast('A', new_order->internal_id, new_order->price, new_order->quantity, new_order->instrument_id, new_order->side == Side::BUY ? 'B' : 'S');
                send_drop_copy(new_order->client_order_id, new_order->internal_id, new_order->price, new_order->quantity, new_order->instrument_id, new_order->side, OrderState::NEW);
                if (price > cached_best_bid) cached_best_bid = price;
            } else pool->deallocate(new_order);
        } else {
            int32_t p = cached_best_bid;
            while (p != -1 && p >= (int32_t)price && new_order->quantity > 0) {
                Limit& level = bids[p];
                while (new_order->quantity > 0 && level.head) {
                    Order* resting = level.head;
                    if (resting->next) [[likely]] {
                        __builtin_prefetch(resting->next, 0, 3);
                    }
                    uint32_t match_qty = std::min(new_order->quantity, resting->quantity);
                    broadcast('E', resting->internal_id, resting->price, match_qty, resting->instrument_id, resting->side == Side::BUY ? 'B' : 'S');
                    broadcast('E', new_order->internal_id, resting->price, match_qty, new_order->instrument_id, new_order->side == Side::BUY ? 'B' : 'S');
                    
                    send_drop_copy(new_order->client_order_id, new_order->internal_id, resting->price, match_qty, new_order->instrument_id, new_order->side, 
                                   new_order->quantity > resting->quantity ? OrderState::PARTIAL_FILL : OrderState::FILLED);
                    send_drop_copy(resting->client_order_id, resting->internal_id, resting->price, match_qty, resting->instrument_id, resting->side, 
                                   resting->quantity > new_order->quantity ? OrderState::PARTIAL_FILL : OrderState::FILLED);

                    if (new_order->quantity >= resting->quantity) [[likely]] {
                        new_order->quantity -= resting->quantity;
                        orders_by_id[resting->internal_id] = nullptr;
                        level.remove(resting);
                        pool->deallocate(resting);
                    } else {
                        resting->quantity -= new_order->quantity;
                        new_order->quantity = 0;
                    }
                }
                if (!level.head) [[likely]] {
                    clear_bit(bids_bitmap, bids_l2, p);
                    if (p > 0) {
                        int32_t next = find_next_bid(p - 1);
                        if (next == -1) {
                            p = -1;
                        } else {
                            cached_best_bid = next;
                            p = next;
                        }
                    } else p = -1;
                } else break;
            }
            if (new_order->quantity > 0) [[likely]] {
                bool is_new_level = !asks[price].head;
                asks[price].add(new_order);
                if (is_new_level) [[unlikely]] {
                    set_bit(asks_bitmap, asks_l2, price);
                }
                orders_by_id[new_order->internal_id] = new_order;
                broadcast('A', new_order->internal_id, new_order->price, new_order->quantity, new_order->instrument_id, new_order->side == Side::BUY ? 'B' : 'S');
                send_drop_copy(new_order->client_order_id, new_order->internal_id, new_order->price, new_order->quantity, new_order->instrument_id, new_order->side, OrderState::NEW);
                if (price < cached_best_ask) cached_best_ask = price;
            } else pool->deallocate(new_order);
        }
    }
};
