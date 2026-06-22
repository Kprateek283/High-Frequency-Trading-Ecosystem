#pragma once
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <vector>
#include <atomic>
#include "../../include/network/IExchangeConnector.h"

#include <simdjson.h>
#include <curl/curl.h>

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

extern std::atomic<bool> global_running;

class CryptoPaperConnector : public IExchangeConnector {
private:
    bool is_connected = false;
    std::thread ws_thread;
    ExecutionCallback exec_callback;
    TickCallback tick_callback;

public:
    ~CryptoPaperConnector() {
        if (ws_thread.joinable()) {
            ws_thread.join();
        }
    }

    void set_execution_callback(ExecutionCallback on_fill) override {
        exec_callback = on_fill;
    }

    bool initialize() override {
        curl_global_init(CURL_GLOBAL_ALL);
        std::cout << "[CryptoConnector] Connecting to Binance Testnet WSS via Boost.Beast...\n";
        
        // Spin up the background thread to handle WSS
        ws_thread = std::thread([this]() {
            this->run_websocket_stream();
        });

        // Wait until connected
        while (!is_connected && global_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        return true;
    }

    void run_websocket_stream() {
        try {
            net::io_context ioc;
            ssl::context ctx{ssl::context::tlsv12_client};

            // This holds the root certificate used for verification
            ctx.set_default_verify_paths();
            ctx.set_verify_mode(ssl::verify_none); // In production, verify peer!

            tcp::resolver resolver{ioc};
            websocket::stream<beast::ssl_stream<tcp::socket>> ws{ioc, ctx};

            auto const results = resolver.resolve("stream.binance.com", "9443");
            auto ep = net::connect(get_lowest_layer(ws), results);

            // Set SNI Hostname (many hosts need this to handshake successfully)
            if(! SSL_set_tlsext_host_name(ws.next_layer().native_handle(), "stream.binance.com"))
            {
                boost::system::error_code ec{static_cast<int>(::ERR_get_error()), boost::asio::error::get_ssl_category()};
                throw boost::system::system_error{ec};
            }

            // Perform the SSL handshake
            ws.next_layer().handshake(ssl::stream_base::client);

            // Perform the websocket handshake
            std::string host = "stream.binance.com:" + std::to_string(ep.port());
            ws.handshake(host, "/ws/btcusdt@depth@100ms");

            std::cout << "[CryptoConnector] Successfully authenticated to Binance WSS.\n";
            is_connected = true;

            beast::flat_buffer buffer;
            simdjson::ondemand::parser parser;
            
            // Benchmarking state
            std::vector<uint64_t> latencies_ns;
            latencies_ns.reserve(100000); // Reserve memory for 100k ticks to avoid allocations

            while(global_running) {
                ws.read(buffer);
                
                auto tick_start = std::chrono::high_resolution_clock::now();
                
                std::string msg = beast::buffers_to_string(buffer.data());
                buffer.consume(buffer.size());

                if (tick_callback) {
                    try {
                        auto doc = parser.iterate(msg);
                        
                        // Route Bids
                        for (auto bid : doc["b"]) {
                            NormalizedTick tick;
                            tick.instrument_id = 0; // BTCUSDT
                            tick.is_trade = false;
                            tick.is_bid = true;
                            
                            std::string_view price_str = bid.at(0).get_string();
                            tick.price = std::stod(std::string(price_str)) * 10000;
                            
                            std::string_view qty_str = bid.at(1).get_string();
                            tick.quantity = std::stod(std::string(qty_str)) * 10000;
                            
                            if (tick.quantity > 0) tick_callback(tick);
                        }

                        // Route Asks
                        for (auto ask : doc["a"]) {
                            NormalizedTick tick;
                            tick.instrument_id = 0; 
                            tick.is_trade = false;
                            tick.is_bid = false;
                            
                            std::string_view price_str = ask.at(0).get_string();
                            tick.price = std::stod(std::string(price_str)) * 10000;
                            
                            std::string_view qty_str = ask.at(1).get_string();
                            tick.quantity = std::stod(std::string(qty_str)) * 10000;
                            
                            if (tick.quantity > 0) tick_callback(tick);
                        }
                    } catch (...) {
                        // Handle parse err
                    }
                }
                
                auto tick_end = std::chrono::high_resolution_clock::now();
                uint64_t latency = std::chrono::duration_cast<std::chrono::nanoseconds>(tick_end - tick_start).count();
                latencies_ns.push_back(latency);
            }

            // Print Percentile Benchmarks on Exit
            if (!latencies_ns.empty()) {
                std::sort(latencies_ns.begin(), latencies_ns.end());
                size_t n = latencies_ns.size();
                std::cout << "\n========================================\n";
                std::cout << " Crypto WSS Translation Latency Metrics \n";
                std::cout << "========================================\n";
                std::cout << "Total Ticks Analyzed: " << n << "\n";
                std::cout << "P50    : " << latencies_ns[n * 0.50] << " ns\n";
                std::cout << "P90    : " << latencies_ns[n * 0.90] << " ns\n";
                std::cout << "P99    : " << latencies_ns[n * 0.99] << " ns\n";
                std::cout << "P99.9  : " << latencies_ns[n * 0.999] << " ns\n";
                std::cout << "Max    : " << latencies_ns[n - 1] << " ns\n";
                std::cout << "========================================\n";
            }

            // Close
            ws.close(websocket::close_code::normal);

        } catch(std::exception const& e) {
            std::cerr << "[CryptoConnector] WSS Error: " << e.what() << "\n";
            is_connected = false;
        }
    }

    void start_market_data(TickCallback on_tick) override {
        tick_callback = on_tick;
        // The event loop is already running in ws_thread, so we just block here.
        while (global_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    void send_order(const NormalizedOrderAction& action) override {
        if (!is_connected) return;

        CURL* curl = curl_easy_init();
        if (curl) {
            std::string url = "https://testnet.binance.vision/api/v3/order";
            std::string payload = "symbol=BTCUSDT&side=" + std::string(action.is_buy ? "BUY" : "SELL") + 
                                  "&type=LIMIT&timeInForce=GTC&quantity=" + std::to_string(action.quantity / 10000.0) + 
                                  "&price=" + std::to_string(action.price / 10000.0) +
                                  "&newClientOrderId=" + std::to_string(action.internal_order_id);
            
            if (action.is_cancel) {
                curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
                payload = "symbol=BTCUSDT&origClientOrderId=" + std::to_string(action.internal_order_id);
            }
            
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
            
            // Execute HTTP POST (blocking in this mock, should be async multiplexed in prod)
            curl_easy_perform(curl);
            curl_easy_cleanup(curl);
            
            // Loopback ACK to our Order Tracker via ExecutionEngine
            if (!action.is_cancel && exec_callback) {
                exec_callback(action.internal_order_id, action.quantity);
            }
        }
    }
};
