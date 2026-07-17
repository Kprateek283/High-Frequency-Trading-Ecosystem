#include <iostream>
#include <thread>
#include <vector>
#include <array>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <atomic>
#include "protocol/messages.h"

// The single instrument this market maker quotes. It used to send "AAPL    ",
// which the gateway's decode never accepted, so every quote was rejected (A1).
constexpr uint16_t MM_INSTRUMENT = 0;   // STK00000

class MarketMaker {
private:
    int tcp_fd;
    int udp_fd;
    struct sockaddr_in mcast_addr;
    
    std::array<uint32_t, 200000> bids_qty;
    std::array<uint32_t, 200000> asks_qty;
    uint32_t best_bid = 0;
    uint32_t best_ask = 199999;
    
    std::atomic<bool> running{true};
    uint64_t my_order_id_counter = 5000000; 

    int64_t position = 0;
    int64_t cash = 0;

public:
    MarketMaker(const char* exchange_ip, int tcp_port) {
        bids_qty.fill(0);
        asks_qty.fill(0);

        tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in serv_addr;
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(tcp_port);
        inet_pton(AF_INET, exchange_ip, &serv_addr.sin_addr);
        
        if (connect(tcp_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
            throw std::runtime_error("Failed to connect to Exchange via TCP");
        }
        std::cout << "Connected to Exchange TCP Gateway." << std::endl;

        udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
        int reuse = 1;
        setsockopt(udp_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        
        struct sockaddr_in local_addr;
        std::memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sin_family = AF_INET;
        local_addr.sin_port = htons(12345);
        local_addr.sin_addr.s_addr = INADDR_ANY;
        bind(udp_fd, (struct sockaddr*)&local_addr, sizeof(local_addr));

        struct ip_mreq mreq;
        mreq.imr_multiaddr.s_addr = inet_addr("239.255.0.1");
        mreq.imr_interface.s_addr = INADDR_ANY;
        setsockopt(udp_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
        std::cout << "Joined Market Data Multicast Group." << std::endl;
    }

    void send_order(Side side, uint64_t price, uint32_t qty) {
        OuchEnterOrder req;
        req.msg_type = 'O';
        std::string token = std::to_string(my_order_id_counter++);
        token.insert(token.begin(), 14 - token.length(), '0');
        std::memcpy(req.order_token, token.c_str(), 14);
        
        req.side = (side == Side::BUY) ? 'B' : 'S';
        req.shares = qty;
        encode_symbol(req.stock, MM_INSTRUMENT);
        req.price = price;
        req.time_in_force = 99998;
        std::memcpy(req.firm, "MM01", 4);
        req.display = 'Y';
        req.capacity = 'P';
        req.iso_eligibility = 'N';
        req.min_quantity = 0;
        req.cross_type = 'N';
        req.customer_type = 'R';
        
        send(tcp_fd, &req, sizeof(req), 0);
    }

    void run() {
        std::thread listener(&MarketMaker::listen_to_market_data, this);
        
        while (running) {
            if (best_bid > 0 && best_ask < 199999) {
                if (best_ask > best_bid) {
                    uint64_t spread = best_ask - best_bid;
                    if (spread > 10) {
                        // Strategy speed increased
                        send_order(Side::BUY, best_bid + 1, 10);
                        send_order(Side::SELL, best_ask - 1, 10);
                        std::cout << "[Market Maker] Quoting tighter spread: BUY " << best_bid + 1 << " | SELL " << best_ask - 1 << std::endl;
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                } else {
                    send_order(Side::BUY, best_ask, 10);
                    send_order(Side::SELL, best_bid, 10);
                    std::cout << "[Market Maker] Crossing book: BUY " << best_ask << " | SELL " << best_bid << std::endl;
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            } else {
                // If book is empty, send seed quotes to start the market
                send_order(Side::BUY, 50000, 10);
                send_order(Side::SELL, 50020, 10);
                std::cout << "[Market Maker] Book empty. Seed quotes sent." << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        listener.join();
    }

private:
    void listen_to_market_data() {
        char buffer[65536]; // Large buffer to receive batches
        while (running) {
            ssize_t n = recv(udp_fd, buffer, sizeof(buffer), 0);
            if (n >= (ssize_t)sizeof(ItchMessage)) {
                // Process all reports in the batch
                for (size_t i = 0; i <= (size_t)n - sizeof(ItchMessage); i += sizeof(ItchMessage)) {
                    ItchMessage* rep = reinterpret_cast<ItchMessage*>(buffer + i);
                    update_local_book(rep);
                    update_pnl(rep);
                }
            }
        }
    }

    void update_pnl(ItchMessage* rep) {
        // We only track PnL for executions
        if (rep->msg_type == 'E') {
            // Note: client_order_id is dropped in true ITCH, but since this is our local book 
            // reconstruction we just naively pretend we can track it or we omit client_id tracking.
            // Since we don't have client_id in ItchMessage natively without tracking internal_id mapping:
            if (rep->side == 'B') {
                position += rep->shares;
                cash -= (rep->price * rep->shares);
            } else {
                position -= rep->shares;
                cash += (rep->price * rep->shares);
            }
            std::cout << "[PnL Report] Pos: " << position << " | Cash: " << cash << " | Last Price: " << rep->price << std::endl;
        }
    }

    void update_local_book(ItchMessage* rep) {
        if (rep->price >= 200000) return;
        uint32_t price = rep->price;

        if (rep->msg_type == 'A') {
            if (rep->side == 'B') {
                bids_qty[price] += rep->shares;
                if (price > best_bid) best_bid = price;
            } else {
                asks_qty[price] += rep->shares;
                if (price < best_ask) best_ask = price;
            }
        } else if (rep->msg_type == 'E' || rep->msg_type == 'X') {
            if (rep->side == 'B') {
                if (bids_qty[price] <= rep->shares) bids_qty[price] = 0;
                else bids_qty[price] -= rep->shares;

                if (bids_qty[best_bid] == 0 && best_bid == price) {
                    while (best_bid > 0 && bids_qty[best_bid] == 0) best_bid--;
                }
            } else {
                if (asks_qty[price] <= rep->shares) asks_qty[price] = 0;
                else asks_qty[price] -= rep->shares;

                if (asks_qty[best_ask] == 0 && best_ask == price) {
                    while (best_ask < 199999 && asks_qty[best_ask] == 0) best_ask++;
                }
            }
        }
    }
};

int main() {
    try {
        MarketMaker mm("127.0.0.1", 9091);
        mm.run();
    } catch (const std::exception& e) {
        std::cerr << "Market Maker Error: " << e.what() << std::endl;
    }
    return 0;
}
