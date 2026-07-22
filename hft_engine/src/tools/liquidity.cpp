// Deterministic crossing driver: produces matches the benchmark can point at.
//
// One TCP connection cannot generate a fill anymore -- self-trade prevention
// rejects an order that would cross the same client's resting order. So this
// opens TWO connections (two server-assigned client_ids): connection A rests a
// column of SELLs, connection B crosses them with BUYs at the same price. The
// resting order at each level always belongs to the *other* client, so STP
// never fires and every pair trades. Reuses the canonical wire struct, so there
// is no second copy of the OUCH layout to drift.
//
// Not a realistic market -- it exists only to make "matches > 0" true and
// reproducible for the Phase 3 gate. Realistic flow is the trading firm.

#include <iostream>
#include <cstring>
#include <cstdlib>
#include <string>
#include <chrono>
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <x86intrin.h>
#include "protocol/messages.h"

// Stamp the wire timestamps right before send so the gateway records latency
// samples (it skips any order with a zero timestamp). liquidity has no firm-side
// pipeline, so all four carry one send-time TSC: the intra-firm stages (t2-t1,
// t3-t2, t4-t3) read ~0 by design, while the TCP path (t5-t4) and end-to-end
// (t5-t1) are the real, meaningful numbers. Firm-pipeline latency is trading_firm.
static inline void stamp(OuchEnterOrder& o) {
    unsigned aux;
    uint64_t ts = __rdtscp(&aux);
    o.t1_exchange_send = o.t2_trading_recv = o.t3_trading_enq = o.t4_network_deq = ts;
}

static int connect_gateway(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "liquidity: connect to 127.0.0.1:" << port << " failed\n";
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

// Fill a NEW order with a 14-digit zero-padded token.
static OuchEnterOrder make_order(uint64_t token, char side, uint16_t sym,
                                 uint32_t price, uint32_t shares) {
    OuchEnterOrder o{};
    o.msg_type = 'O';
    std::string t = std::to_string(token);
    t.insert(t.begin(), 14 - t.length(), '0');
    std::memcpy(o.order_token, t.c_str(), 14);
    o.side = side;
    o.shares = shares;
    encode_symbol(o.stock, sym);
    o.price = price;
    o.time_in_force = 99998;
    std::memcpy(o.firm, "LIQ1", 4);
    o.display = 'Y';
    o.capacity = 'P';
    o.iso_eligibility = 'N';
    o.min_quantity = 0;
    o.cross_type = 'N';
    o.customer_type = 'R';
    return o;
}

int main() {
    const char* n_env = std::getenv("LIQUIDITY_ORDERS");
    int n = n_env ? std::atoi(n_env) : 10000;   // crossing pairs → matches
    if (n < 1) n = 1;
    const uint32_t price = 50000;
    const uint16_t sym = 0;                       // STK00000, one shard

    int rester = connect_gateway(9091);
    int crosser = connect_gateway(9091);
    if (rester < 0 || crosser < 0) return 1;

    // A: rest N asks. Small pause so they are in the book before the buys land
    // (not required for correctness -- either side crossing the other fills --
    // but it makes the resting side deterministic).
    for (int i = 0; i < n; ++i) {
        OuchEnterOrder a = make_order(1 + i, 'S', sym, price, 10);
        stamp(a);
        if (send(rester, &a, sizeof(a), 0) != (ssize_t)sizeof(a)) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // B: cross every one of them.
    for (int i = 0; i < n; ++i) {
        OuchEnterOrder b = make_order(10000000 + i, 'B', sym, price, 10);
        stamp(b);
        if (send(crosser, &b, sizeof(b), 0) != (ssize_t)sizeof(b)) break;
    }

    std::cout << "liquidity: sent " << n << " SELL + " << n
              << " BUY at " << price << " on STK00000 (expect " << n
              << " matches)\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // let the engine drain
    close(rester);
    close(crosser);
    return 0;
}
