#pragma once
#include <charconv>
#include <cstring>
#include <iostream>
#include <atomic>
#include <vector>
#include <algorithm>
#include <chrono>
#include <x86intrin.h>
#include <thread>
#include "../../include/network/IExchangeConnector.h"
#include "tcp_client.h"
#include "udp_listener.h"
#include "protocol/messages.h"   // canonical wire schema, from hft_engine/src
#include "../utils/spsc_queue.h"

extern std::atomic<bool> global_running;

class LocalExchangeConnector : public IExchangeConnector {
private:
    TCPClient tcp_client;
    UDPListener* udp_listener = nullptr;
    std::atomic<bool> running{false};

    SPSCQueue<NormalizedOrderAction, 1024> order_queue;
    
    std::atomic<uint64_t> total_serialize_cycles{0};
    std::atomic<uint64_t> total_send_cycles{0};
    std::atomic<uint64_t> total_enqueue_cycles{0};
    std::atomic<uint64_t> total_dequeue_cycles{0};
    std::atomic<uint64_t> total_orders_sent{0};

    const char* INSTRUMENT_STRINGS[4] = {
        "INSTR0  ",
        "INSTR1  ",
        "INSTR2  ",
        "INSTR3  "
    };

    ExecutionCallback exec_callback;
    std::thread tcp_rx_thread;
    std::thread network_tx_thread;

public:
    ~LocalExchangeConnector() {
        if (udp_listener) delete udp_listener;
        if (tcp_rx_thread.joinable()) tcp_rx_thread.join();
        if (network_tx_thread.joinable()) network_tx_thread.join();
    }
    
    LocalExchangeConnector() {
        occupancies.reserve(5000000);
    }

    void set_execution_callback(ExecutionCallback on_fill) override {
        exec_callback = on_fill;
    }

    int tcp_mode = 2;
    int num_connections = 1;
    std::vector<std::unique_ptr<TCPClient>> tcp_clients;

    bool initialize() override {
        const char* mode_env = std::getenv("TCP_MODE");
        if (mode_env) tcp_mode = std::atoi(mode_env);

        const char* threads_env = std::getenv("GATEWAY_THREADS");
        if (threads_env) num_connections = std::atoi(threads_env);
        if (num_connections < 1) num_connections = 1;

        try {
            // Bind to Multicast ITCH Group FIRST so we don't miss packets when TCP connects
            udp_listener = new UDPListener("239.255.0.1", 12345);
        } catch (const std::exception& e) {
            std::cerr << "[LocalConnector] UDP Failed: " << e.what() << "\n";
            return false;
        }

        bool blocking = (tcp_mode == 1);
        for (int i = 0; i < num_connections; ++i) {
            auto client = std::make_unique<TCPClient>();
            if (!client->connect_to("127.0.0.1", 9091, blocking)) {
                std::cerr << "[LocalConnector] Failed to connect to Exchange TCP Gateway. Connection: " << i << "\n";
                return false;
            }
            tcp_clients.push_back(std::move(client));
        }

        // Start TCP polling thread for OUCH Execution Reports
        tcp_rx_thread = std::thread([this]() {
            while (global_running) {
                for (auto& client : tcp_clients) {
                    if (!client->is_connected()) continue;
                    client->poll([this](const char* buf, size_t len) {
                        size_t offset = 0;
                        while (offset < len) {
                            char msg_type = buf[offset];
                            if (msg_type == 'E') {
                                if (offset + sizeof(OuchExecutionReport) <= len) {
                                    const OuchExecutionReport* rep = reinterpret_cast<const OuchExecutionReport*>(buf + offset);
                                    if (exec_callback) {
                                        char token_str[15];
                                        std::memcpy(token_str, rep->order_token, 14);
                                        token_str[14] = '\0';
                                        uint64_t order_id = std::stoull(token_str);
                                        exec_callback(order_id, rep->executed_shares); 
                                    }
                                    offset += sizeof(OuchExecutionReport);
                                } else {
                                    break;
                                }
                            } else {
                                // Unhandled msg type, break to avoid infinite loop
                                break;
                            }
                        }
                    });
                }
            }
        });

        // Start Network TX Thread
        network_tx_thread = std::thread([this]() {
            std::vector<std::vector<OuchEnterOrder>> batches(num_connections);
            for (auto& b : batches) b.resize(64);
            std::vector<int> batch_counts(num_connections, 0);

            while (global_running) {
                if (tcp_mode == 3) { // BATCHED
                    int total_processed = 0;
                    unsigned aux;
                    while (total_processed < 64) {
                        NormalizedOrderAction action;
                        if (!order_queue.dequeue(action)) break;
                        
                        if (!action.is_cancel) {
                            int conn_idx = action.instrument_id % num_connections; // distribute load
                            int& count = batch_counts[conn_idx];
                            
                            OuchEnterOrder& req = batches[conn_idx][count];
                            req.msg_type = 'O';
                            std::memset(req.order_token, '0', 14);
                            char temp[14];
                            auto [ptr, ec] = std::to_chars(temp, temp + 14, action.internal_order_id);
                            size_t len = ptr - temp;
                            std::memcpy(req.order_token + (14 - len), temp, len);
                            
                            req.side = action.is_buy ? 'B' : 'S';
                            req.shares = action.quantity;
                            
                            if (action.instrument_id < 4) [[likely]] {
                                std::memcpy(req.stock, INSTRUMENT_STRINGS[action.instrument_id], 8);
                            } else {
                                std::memcpy(req.stock, "UNKNOWN ", 8);
                            }
                            
                            req.price = action.price;
                            req.time_in_force = 99998;
                            std::memcpy(req.firm, "HFT1", 4);
                            req.display = 'Y';
                            req.capacity = 'P';
                            req.iso_eligibility = 'Y';
                            req.min_quantity = 0;
                            req.cross_type = 'N';
                            req.customer_type = 'R';

                            req.t1_exchange_send = action.t1_exchange_send;
                            req.t2_trading_recv = action.t2_trading_recv;
                            req.t3_trading_enq = action.t3_trading_enq;
                            req.t4_network_deq = __rdtscp(&aux);

                            count++;
                            total_processed++;
                        }
                    }
                    
                    if (total_processed > 0) {
                        uint64_t t1 = __rdtscp(&aux);
                        for (int i = 0; i < num_connections; ++i) {
                            if (batch_counts[i] > 0) {
                                size_t total_bytes = batch_counts[i] * sizeof(OuchEnterOrder);
                                while (!tcp_clients[i]->send_bytes(batches[i].data(), total_bytes)) {
                                    if (!tcp_clients[i]->is_connected()) break;
                                    __builtin_ia32_pause();
                                }
                                total_orders_sent += batch_counts[i];
                                batch_counts[i] = 0;
                            }
                        }
                        uint64_t t2 = __rdtscp(&aux);
                        total_send_cycles += (t2 - t1);
                    }

                } else { // Mode 1 or 2
                    NormalizedOrderAction action;
                    unsigned aux;
                    
                    uint64_t start_deq = __rdtscp(&aux);
                    bool has_item = order_queue.dequeue(action);
                    uint64_t end_deq = __rdtscp(&aux);
                    
                    if (has_item) {
                        total_dequeue_cycles += (end_deq - start_deq);
                        uint64_t t1 = __rdtscp(&aux);
                        int conn_idx = action.instrument_id % num_connections;
                        
                        if (!action.is_cancel) {
                            OuchEnterOrder req;
                            req.msg_type = 'O';

                            std::memset(req.order_token, '0', 14);
                            char temp[14];
                            auto [ptr, ec] = std::to_chars(temp, temp + 14, action.internal_order_id);
                            size_t len = ptr - temp;
                            std::memcpy(req.order_token + (14 - len), temp, len);
                            
                            req.side = action.is_buy ? 'B' : 'S';
                            req.shares = action.quantity;
                            
                            if (action.instrument_id < 4) [[likely]] {
                                std::memcpy(req.stock, INSTRUMENT_STRINGS[action.instrument_id], 8);
                            } else {
                                std::memcpy(req.stock, "UNKNOWN ", 8);
                            }
                            
                            req.price = action.price;
                            req.time_in_force = 99998; // DAY
                            std::memcpy(req.firm, "HFT1", 4);
                            req.display = 'Y';
                            req.capacity = 'P';
                            req.iso_eligibility = 'Y';
                            req.min_quantity = 0;
                            req.cross_type = 'N';
                            req.customer_type = 'R';

                            req.t1_exchange_send = action.t1_exchange_send;
                            req.t2_trading_recv = action.t2_trading_recv;
                            req.t3_trading_enq = action.t3_trading_enq;
                            req.t4_network_deq = end_deq;

                            uint64_t t2 = __rdtscp(&aux);
                            while (!tcp_clients[conn_idx]->send_bytes(&req, sizeof(req))) {
                                if (!tcp_clients[conn_idx]->is_connected()) break;
                                __builtin_ia32_pause();
                            }
                            uint64_t t3 = __rdtscp(&aux);
                            
                            total_serialize_cycles += (t2 - t1);
                            total_send_cycles += (t3 - t2);
                            total_orders_sent++;
                        } else {
                            OuchCancelOrder req;
                            req.msg_type = 'X';

                            std::memset(req.order_token, '0', 14);
                            char temp[14];
                            auto [ptr, ec] = std::to_chars(temp, temp + 14, action.internal_order_id);
                            size_t len = ptr - temp;
                            std::memcpy(req.order_token + (14 - len), temp, len);
                            
                            req.shares = action.quantity;

                            uint64_t t2 = __rdtscp(&aux);
                            while (!tcp_clients[conn_idx]->send_bytes(&req, sizeof(req))) {
                                if (!tcp_clients[conn_idx]->is_connected()) break;
                                __builtin_ia32_pause();
                            }
                            uint64_t t3 = __rdtscp(&aux);
                            
                            total_serialize_cycles += (t2 - t1);
                            total_send_cycles += (t3 - t2);
                            total_orders_sent++;
                        }
                    }
                }
            }
        });

        return true;
    }

    void start_market_data(TickCallback on_tick) override {
        if (!udp_listener) return;

        // Benchmarking state
        std::vector<uint64_t> latencies_ns;
        std::vector<uint64_t> latencies_cycles;
        latencies_ns.reserve(500000); 
        latencies_cycles.reserve(500000);

        auto total_start = std::chrono::high_resolution_clock::now();
        uint64_t total_ticks = 0;

        // Start listening to UDP loop indefinitely
        while (global_running) {
            udp_listener->spin([this, on_tick, &latencies_ns, &latencies_cycles, &total_ticks](const char* buf, size_t len) {
                
                size_t num_msgs = len / sizeof(ItchMessage);
                const ItchMessage* msgs = reinterpret_cast<const ItchMessage*>(buf);

                for (size_t i = 0; i < num_msgs; ++i) {
                    const ItchMessage& raw = msgs[i];
                    
                    NormalizedTick tick;
                    tick.instrument_id = raw.stock_locate;
                    tick.price = raw.price;
                    tick.quantity = raw.shares;
                    tick.is_bid = (raw.side == 'B');
                    tick.is_trade = (raw.msg_type == 'E'); // 'E' means Execution

                    unsigned aux;
                    uint64_t start_cycles = __rdtscp(&aux);
                    
                    tick.t1_exchange_send = raw.timestamp;
                    tick.t2_trading_recv = start_cycles;
                    
                    auto tick_start = std::chrono::high_resolution_clock::now();
                    
                    on_tick(tick);
                    
                    auto tick_end = std::chrono::high_resolution_clock::now();
                    uint64_t end_cycles = __rdtscp(&aux);

                    uint64_t latency = std::chrono::duration_cast<std::chrono::nanoseconds>(tick_end - tick_start).count();
                    latencies_ns.push_back(latency);
                    latencies_cycles.push_back(end_cycles - start_cycles);
                    total_ticks++;
                }
            });
        }
        auto total_end = std::chrono::high_resolution_clock::now();
        uint64_t total_runtime_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(total_end - total_start).count();
        
        // Print Percentile Benchmarks on Exit
        if (!latencies_ns.empty()) {
            std::sort(latencies_ns.begin(), latencies_ns.end());
            std::sort(latencies_cycles.begin(), latencies_cycles.end());
            size_t n = latencies_ns.size();
            std::cout << "\n========================================\n";
            std::cout << " Local Binary (ITCH) Translation Latency Metrics \n";
            std::cout << "========================================\n";
            std::cout << "Total Ticks Analyzed: " << n << "\n";
            std::cout << "Total Bulk Runtime (ns): " << total_runtime_ns << "\n";
            std::cout << "Bulk average (ns/tick) : " << (total_runtime_ns / total_ticks) << " ns\n";
            std::cout << "--- std::chrono::nanoseconds ---\n";
            std::cout << "P50    : " << latencies_ns[n * 0.50] << " ns\n";
            std::cout << "P90    : " << latencies_ns[n * 0.90] << " ns\n";
            std::cout << "P99    : " << latencies_ns[n * 0.99] << " ns\n";
            std::cout << "P99.9  : " << latencies_ns[n * 0.999] << " ns\n";
            std::cout << "Max    : " << latencies_ns[n - 1] << " ns\n";
            std::cout << "--- __rdtscp Cycles ---\n";
            std::cout << "P50    : " << latencies_cycles[n * 0.50] << " cycles\n";
            std::cout << "P90    : " << latencies_cycles[n * 0.90] << " cycles\n";
            std::cout << "P99    : " << latencies_cycles[n * 0.99] << " cycles\n";
            std::cout << "P99.9  : " << latencies_cycles[n * 0.999] << " cycles\n";
            std::cout << "Max    : " << latencies_cycles[n - 1] << " cycles\n";
            std::cout << "========================================\n";
            std::cout << " Execution Breakdown (Average per Order) \n";
            std::cout << "========================================\n";
            uint64_t sent = total_orders_sent.load();
            if (sent > 0) {
                std::cout << "Enqueue (Trading) : " << (total_enqueue_cycles.load() / sent) << " cycles\n";
                std::cout << "Dequeue (Network) : " << (total_dequeue_cycles.load() / sent) << " cycles\n";
                std::cout << "Serialization     : " << (total_serialize_cycles.load() / sent) << " cycles\n";
                std::cout << "Socket send()     : " << (total_send_cycles.load() / sent) << " cycles\n";
                std::cout << "Total orders      : " << sent << "\n";
            } else {
                std::cout << "No orders sent.\n";
            }
            std::cout << "========================================\n";
            if (!occupancies.empty()) {
                std::sort(occupancies.begin(), occupancies.end());
                std::cout << " SPSC Queue Occupancy Metrics \n";
                std::cout << "========================================\n";
                std::cout << "P50    : " << occupancies[sent * 0.50] << " items\n";
                std::cout << "P90    : " << occupancies[sent * 0.90] << " items\n";
                std::cout << "P99    : " << occupancies[sent * 0.99] << " items\n";
                std::cout << "Max    : " << occupancies[sent - 1] << " items\n";
                std::cout << "========================================\n";
            }
        }
    }

    // Benchmark state for occupancy
    std::vector<uint64_t> occupancies;

    void send_order(const NormalizedOrderAction& action_ref) override {
        unsigned aux;
        uint64_t t1 = __rdtscp(&aux);
        NormalizedOrderAction action = action_ref;
        action.t3_trading_enq = t1;
        
        occupancies.push_back(order_queue.size());

        while (!order_queue.enqueue(action)) {
            // Spin if queue is full (backpressure)
        }
        
        uint64_t t2 = __rdtscp(&aux);
        total_enqueue_cycles += (t2 - t1);
    }
};
