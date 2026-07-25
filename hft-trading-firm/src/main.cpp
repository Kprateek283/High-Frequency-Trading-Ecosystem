#include <iostream>
#include <thread>
#include <atomic>
#include <csignal>
#include <vector>
#include <memory>
#include <string>
#include <chrono>
#include <cstdlib>
#include "network/LocalExchangeConnector.h"
#ifdef WITH_CRYPTO
#include "network/CryptoPaperConnector.h"   // needs simdjson/curl/OpenSSL; opt-in via -DWITH_CRYPTO
#endif
#include "data/book_builder.h"
#include "signal/signal_engine.h"
#include "strategy/market_maker.h"
#include "risk/risk_manager.h"
#include "execution/execution_engine.h"
#include "monitoring/firm_stats.h"

std::atomic<bool> global_running{true};

void signal_handler(int signum) {
    std::cout << "\n[Firm System] Shutdown signal (" << signum << ") received.\n";
    global_running = false;
}

int main(int argc, char** argv) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "========================================\n";
    std::cout << " HFT Trading Firm System Starting...\n";
    std::cout << "========================================\n";

    std::string venue = (argc > 1) ? argv[1] : "local";
    std::unique_ptr<IExchangeConnector> connector;

    if (venue == "local") {
        connector = std::make_unique<LocalExchangeConnector>();
        std::cout << "[System] Using Local Exchange Simulator Connector.\n";
    } else if (venue == "crypto") {
#ifdef WITH_CRYPTO
        connector = std::make_unique<CryptoPaperConnector>();
        std::cout << "[System] Using Crypto Paper Trading Connector.\n";
#else
        std::cerr << "[Error] Crypto venue not compiled in. Rebuild with -DWITH_CRYPTO=ON.\n";
        return 1;
#endif
    } else {
        std::cerr << "[Error] Unknown venue type: " << venue << "\n";
        return 1;
    }

    try {
        if (!connector->initialize()) {
            std::cerr << "[Error] Failed to initialize connector.\n";
            return 1;
        }

        auto book_builder = std::make_unique<BookBuilder>();
        SignalEngine signal_engine;
        MarketMakerStrategy strategy;
        RiskManager risk_mgr;
        // Heap-allocated: its order_tracker is ~11MB (MAX_LIVE_ORDERS entries),
        // which overflows the 8MB thread stack if placed as a by-value local.
        auto execution_engine = std::make_unique<ExecutionEngine>(connector.get(), &risk_mgr);

        // Firm monitoring region, keyed by FIRM_ID so N firms don't collide.
        // FIRM_STATS_SHM_PATH overrides the full path when set (and non-empty).
        std::string firm_id = "HFT1";
        if (const char* f = std::getenv("FIRM_ID")) firm_id = f;
        std::string firm_stats_path = "/dev/shm/firm_stats_" + firm_id;
        if (const char* p = std::getenv("FIRM_STATS_SHM_PATH")) {
            if (*p) firm_stats_path = p;
        }
        FirmStatsRegion* firm_stats = map_firm_stats_region(firm_stats_path.c_str());
        if (firm_stats)
            std::cout << "[System] Firm stats region: " << firm_stats_path << "\n";
        else
            std::cerr << "[System] Firm stats region unavailable at " << firm_stats_path << "\n";

        std::cout << "[System] All components initialized. Trading active.\n";

        std::vector<NormalizedOrderAction> outbound_actions;
        outbound_actions.reserve(128); // Pre-allocate to prevent heap growth

        // Pre-allocate tracking variables
        uint64_t total_book_cycles = 0;
        uint64_t total_signal_cycles = 0;
        uint64_t total_strategy_cycles = 0;
        uint64_t total_risk_cycles = 0;
        uint64_t total_execution_cycles = 0;
        uint64_t total_ticks = 0;

        // The market-data callback thread is the single seqlock writer; gate the
        // ~100ms publish on wall time (it runs per tick).
        auto last_publish = std::chrono::steady_clock::now();

        connector->start_market_data([&](const NormalizedTick& tick) {
            unsigned aux;
            
            // 1. Update Local Limit Order Book
            uint64_t t1 = __rdtscp(&aux);
            book_builder->process_tick(tick);
            BookSnapshot snap = book_builder->get_snapshot(tick.instrument_id);
            uint64_t t2 = __rdtscp(&aux);

            // 2. Compute Predictive Alpha Signals
            AlphaSignals sig = signal_engine.compute_signals(snap);
            uint64_t t3 = __rdtscp(&aux);

            // 3. Strategy Engine evaluates signals and generates intents
            outbound_actions.clear();
            strategy.on_tick(tick, sig, outbound_actions);
            uint64_t t4 = __rdtscp(&aux);

            // 4. Send intents through Risk to Execution Engine
            uint64_t total_risk_this_tick = 0;
            uint64_t total_exec_this_tick = 0;
            
            for (const auto& action : outbound_actions) {
                uint64_t r1 = __rdtscp(&aux);
                bool safe = risk_mgr.check_order(action);
                uint64_t r2 = __rdtscp(&aux);
                total_risk_this_tick += (r2 - r1);

                if (safe) {
                    uint64_t e1 = __rdtscp(&aux);
                    execution_engine->execute_action(action);
                    uint64_t e2 = __rdtscp(&aux);
                    total_exec_this_tick += (e2 - e1);
                }
            }

            total_book_cycles += (t2 - t1);
            total_signal_cycles += (t3 - t2);
            total_strategy_cycles += (t4 - t3);
            total_risk_cycles += total_risk_this_tick;
            total_execution_cycles += total_exec_this_tick;
            total_ticks++;

            // 5. Publish the firm's state to the seqlock region every ~100ms. This
            // thread is the sole writer; it reads the (now-atomic) shared sources.
            if (firm_stats) {
                auto now = std::chrono::steady_clock::now();
                if (now - last_publish >= std::chrono::milliseconds(100)) {
                    last_publish = now;
                    uint64_t hb = __rdtscp(&aux);
                    ConnectorStats cs = connector->stats();
                    firm_stats_write(firm_stats, [&]() {
                        firm_stats->heartbeat_tsc = hb;
                        firm_stats->kill_switch = risk_mgr.kill_switch_engaged() ? 1u : 0u;
                        firm_stats->realized_pnl = risk_mgr.realized_pnl();
                        firm_stats->orders_sent = cs.orders_sent;
                        firm_stats->orders_acked = execution_engine->orders_acked();
                        firm_stats->in_flight = execution_engine->in_flight();
                        firm_stats->ticks = total_ticks;
                        firm_stats->book_cycles = total_book_cycles;
                        firm_stats->signal_cycles = total_signal_cycles;
                        firm_stats->strategy_cycles = total_strategy_cycles;
                        firm_stats->risk_cycles = total_risk_cycles;
                        firm_stats->exec_cycles = total_execution_cycles;
                        firm_stats->serialize_cycles = cs.serialize_cycles;
                        firm_stats->send_cycles = cs.send_cycles;
                        firm_stats->enqueue_cycles = cs.enqueue_cycles;
                        firm_stats->dequeue_cycles = cs.dequeue_cycles;
                        for (int i = 0; i < FIRM_STATS_MAX_INSTRUMENTS; ++i)
                            firm_stats->position[i] = risk_mgr.position(static_cast<uint16_t>(i));
                    });
                }
            }
        });

        if (total_ticks > 0) {
            std::cout << "\n========================================\n";
            std::cout << " Pipeline Cycle Attribution (Average)\n";
            std::cout << "========================================\n";
            std::cout << "BookBuilder : " << (total_book_cycles / total_ticks) << " cycles\n";
            std::cout << "Signals     : " << (total_signal_cycles / total_ticks) << " cycles\n";
            std::cout << "Strategy    : " << (total_strategy_cycles / total_ticks) << " cycles\n";
            std::cout << "Risk        : " << (total_risk_cycles / total_ticks) << " cycles\n";
            std::cout << "Execution   : " << (total_execution_cycles / total_ticks) << " cycles\n";
            std::cout << "Total/Tick  : " << ((total_book_cycles + total_signal_cycles + total_strategy_cycles + total_risk_cycles + total_execution_cycles) / total_ticks) << " cycles\n";
            std::cout << "========================================\n";
        }

    } catch (const std::exception& e) {
        std::cerr << "[Fatal Error] " << e.what() << "\n";
    }

    std::cout << "[System] Shutting down gracefully...\n";
    return 0;
}


