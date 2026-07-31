#include "gateway_fixture.h"

using namespace gwtest;

void test_ack_coalescing() {
    GatewayFixture f;
    size_t cid = f.connect(1);
    
    int bufsize = 1024;
    setsockopt(f.conns[cid].server_fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    
    std::vector<OutboundAck> injected;
    for (int i = 0; i < 1000; ++i) {
        OutboundAck ack;
        ack.client_id = 1;
        ack.report.msg_type = 'E';
        ack.report.executed_shares = i;
        injected.push_back(ack);
        f.inject_ack(ack);
    }
    
    f.pump_acks();
    
    // Now read from client_fd and see what we get
    int flags = fcntl(f.conns[cid].client_fd, F_GETFL, 0);
    fcntl(f.conns[cid].client_fd, F_SETFL, flags | O_NONBLOCK);
    
    std::vector<OuchExecutionReport> received;
    
    while (true) {
        OuchExecutionReport report;
        ssize_t n = read(f.conns[cid].client_fd, &report, sizeof(report));
        if (n > 0) {
            received.push_back(report);
            f.pump_acks(); // keep pumping as we drain
        } else {
            break; // EAGAIN or EOF
        }
    }
    
    if (received.size() != injected.size()) {
        std::printf("Expected %zu acks, got %zu\n", injected.size(), received.size());
    }
    CHECK(received.size() == injected.size());
    
    bool corrupted = false;
    for (size_t i = 0; i < received.size(); ++i) {
        if (received[i].executed_shares != i) {
            std::printf("Corrupted! Expected %zu got %u\n", i, received[i].executed_shares);
            corrupted = true;
            break;
        }
    }
    CHECK(!corrupted);
    if (!corrupted) {
        std::printf("All %zu acks received intact!\n", received.size());
    }
}
