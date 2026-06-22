#include <iostream>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <chrono>
#include <thread>
#include "protocol/messages.h"

int main() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9091);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Failed to connect to engine" << std::endl;
        return 1;
    }

    int nodelay = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    const int TOTAL_ORDERS = 1000000;
    std::vector<OuchEnterOrder> orders(TOTAL_ORDERS);
    std::memset(orders.data(), 0, TOTAL_ORDERS * sizeof(OuchEnterOrder));

    const char* stocks[4] = {"AAPL    ", "MSFT    ", "GOOG    ", "AMZN    "};

    for (int i = 0; i < TOTAL_ORDERS; ++i) {
        orders[i].msg_type = 'O';
        std::string token = std::to_string(i + 1);
        token.insert(token.begin(), 14 - token.length(), '0');
        std::memcpy(orders[i].order_token, token.c_str(), 14);
        
        orders[i].side = (i % 2 == 0) ? 'B' : 'S';
        orders[i].shares = 10;
        
        int num_symbols = 1000;
        int sym_id = 0;
        const char* wl = std::getenv("WORKLOAD_TYPE");
        int workload = wl ? std::atoi(wl) : 1;
        
        if (workload == 1) { // Sequential
            sym_id = i % num_symbols;
        } else if (workload == 2) { // Uniform Random
            sym_id = rand() % num_symbols;
        } else if (workload == 3) { // Market Realistic Distribution
            int r = rand() % 100;
            if (r < 20) sym_id = 0; // AAPL
            else if (r < 35) sym_id = 1; // MSFT
            else if (r < 50) sym_id = 2; // NVDA
            else if (r < 60) sym_id = 3; // SPY
            else sym_id = 4 + (rand() % (num_symbols - 4));
        } else if (workload == 4) { // Worst Case (random sym, random price)
            sym_id = rand() % num_symbols;
        }
        
        char stock_name[8] = {'S', 'T', 'K', '0', '0', '0', '0', '0'};
        stock_name[7] = '0' + (sym_id % 10);
        stock_name[6] = '0' + ((sym_id / 10) % 10);
        stock_name[5] = '0' + ((sym_id / 100) % 10);
        stock_name[4] = '0' + ((sym_id / 1000) % 10);
        stock_name[3] = '0' + ((sym_id / 10000) % 10);
        std::memcpy(orders[i].stock, stock_name, 8);
        
        if (workload == 4) {
            orders[i].price = 1 + (rand() % 99999);
        } else {
            orders[i].price = 50000 + (i % 100);
        }
        orders[i].time_in_force = 99998;
        std::memcpy(orders[i].firm, "TEST", 4);
        orders[i].display = 'Y';
        orders[i].capacity = 'P';
        orders[i].iso_eligibility = 'N';
        orders[i].min_quantity = 0;
        orders[i].cross_type = 'N';
        orders[i].customer_type = 'R';
    }

    const char* rate_env = std::getenv("TARGET_RATE");
    double target_rate = rate_env ? std::atof(rate_env) : 1000000.0;

    std::cout << "C++ Tester: Sending 1M NEW orders at " << target_rate << " msgs/sec..." << std::endl;
    auto start = std::chrono::high_resolution_clock::now();

    const size_t BATCH = 100;
    for (int i = 0; i < TOTAL_ORDERS; i += BATCH) {
        send(sock, &orders[i], BATCH * sizeof(OuchEnterOrder), 0);
        
        if (target_rate > 0) {
            double expected_seconds = (i + BATCH) / target_rate;
            while (true) {
                auto now = std::chrono::high_resolution_clock::now();
                std::chrono::duration<double> elapsed = now - start;
                if (elapsed.count() >= expected_seconds) {
                    break;
                }
                __builtin_ia32_pause();
            }
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;
    
    std::cout << "Finished sending 1M orders in " << diff.count() << " seconds." << std::endl;
    std::cout << "Throughput: " << (TOTAL_ORDERS / diff.count()) << " orders/sec" << std::endl;

    std::cout << "C++ Tester: Sending 1M CANCEL requests to clean up OrderBook state..." << std::endl;
    std::vector<OuchCancelOrder> cancels(TOTAL_ORDERS);
    std::memset(cancels.data(), 0, TOTAL_ORDERS * sizeof(OuchCancelOrder));
    for (int i = 0; i < TOTAL_ORDERS; ++i) {
        cancels[i].msg_type = 'X';
        std::string token = std::to_string(i + 1);
        token.insert(token.begin(), 14 - token.length(), '0');
        std::memcpy(cancels[i].order_token, token.c_str(), 14);
        cancels[i].shares = 10;
    }
    for (int i = 0; i < TOTAL_ORDERS; i += BATCH) {
        send(sock, &cancels[i], BATCH * sizeof(OuchCancelOrder), 0);
        std::this_thread::sleep_for(std::chrono::microseconds(1));
    }
    std::cout << "Cleanup complete." << std::endl;

    close(sock);
    return 0;
}
