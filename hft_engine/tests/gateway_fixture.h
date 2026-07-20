#pragma once
// A live TCPServer driven over socketpairs instead of a real listener: no
// accept, no port races, and the test controls exactly how bytes are split and
// which connection they arrive on. Shared by test_framing and test_identity.
#include "tests.h"
#include "gateway/tcp_server.h"
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace gwtest {

// A test port that no part of the system uses; the constructor still binds,
// even though these tests never accept on the listener.
constexpr int TEST_PORT = 19091;

struct GatewayFixture {
    std::array<std::vector<std::unique_ptr<LockFreeQueue<EngineTask, 524288>>>, NUM_SHARDS> queues;
    std::vector<std::unique_ptr<LockFreeQueue<DropCopyMessage, 1048576>>> reject_queues;
    std::array<std::unique_ptr<MemoryPool<Order>>, NUM_SHARDS> pools;
    std::unique_ptr<TCPServer> server;
    std::unordered_map<int, TCPServer::ClientState> states;

    // One entry per simulated connection.
    struct Conn {
        int client_fd = -1;   // test writes here
        int server_fd = -1;   // gateway reads here
    };
    std::vector<Conn> conns;

    GatewayFixture() {
        // One gateway worker; every test order targets instrument 0 -> shard 0,
        // but all shards need a pool because the gateway indexes pools[shard].
        for (int s = 0; s < NUM_SHARDS; ++s) {
            pools[s] = std::make_unique<MemoryPool<Order>>(4096);
            queues[s].push_back(std::make_unique<LockFreeQueue<EngineTask, 524288>>());
        }
        reject_queues.push_back(std::make_unique<LockFreeQueue<DropCopyMessage, 1048576>>());
        server = std::make_unique<TCPServer>(TEST_PORT, 1, queues, reject_queues, pools);
    }

    ~GatewayFixture() {
        for (auto& c : conns) {
            if (c.client_fd >= 0) close(c.client_fd);
            // handle_client closes server_fd itself on a framing error; only
            // close it here if it is still open.
            if (c.server_fd >= 0 && fcntl(c.server_fd, F_GETFD) != -1) close(c.server_fd);
        }
        for (int fd : server->listen_fds) close(fd);
        for (int fd : server->epoll_fds) close(fd);
    }

    // Adds a connection and assigns it an identity the way accept() does.
    // Returns its index.
    size_t connect(uint32_t client_id) {
        int fds[2];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        Conn c;
        c.client_fd = fds[0];
        c.server_fd = fds[1];

        // The gateway's read loop runs until EAGAIN, so its end must be
        // non-blocking or handle_client would never return.
        int flags = fcntl(c.server_fd, F_GETFL, 0);
        fcntl(c.server_fd, F_SETFL, flags | O_NONBLOCK);

        states[c.server_fd].client_id = client_id;
        conns.push_back(c);
        return conns.size() - 1;
    }

    void pump(size_t conn = 0) { server->handle_client(conns[conn].server_fd, states, 0); }

    void send_bytes(const void* p, size_t n, size_t conn = 0) {
        CHECK(write(conns[conn].client_fd, p, n) == (ssize_t)n);
    }

    // Drains shard 0's ingress queue and returns how many tasks landed.
    int tasks_enqueued() {
        int n = 0;
        EngineTask t;
        while (queues[0][0]->pop(t)) ++n;
        return n;
    }

    int rejects_enqueued() {
        int n = 0;
        DropCopyMessage d;
        while (reject_queues[0]->pop(d)) ++n;
        return n;
    }

    // Drains and returns shard 0's tasks, so a test can inspect their fields.
    std::vector<EngineTask> drain_tasks() {
        std::vector<EngineTask> out;
        EngineTask t;
        while (queues[0][0]->pop(t)) out.push_back(t);
        return out;
    }

    std::vector<DropCopyMessage> drain_rejects() {
        std::vector<DropCopyMessage> out;
        DropCopyMessage d;
        while (reject_queues[0]->pop(d)) out.push_back(d);
        return out;
    }
};

// A valid order for instrument 0 (STK00000), which routes to shard 0.
inline OuchEnterOrder make_order(uint64_t token_num, uint32_t price, uint32_t shares, char side) {
    OuchEnterOrder o;
    std::memset(&o, 0, sizeof(o));
    o.msg_type = 'O';
    std::string token = std::to_string(token_num);
    token.insert(token.begin(), 14 - token.length(), '0');
    std::memcpy(o.order_token, token.c_str(), 14);
    o.side = side;
    o.shares = shares;
    encode_symbol(o.stock, 0);
    o.price = price;
    o.time_in_force = 99998;
    std::memcpy(o.firm, "HFT1", 4);
    o.display = 'Y';
    o.capacity = 'P';
    o.iso_eligibility = 'N';
    o.cross_type = 'N';
    o.customer_type = 'R';
    return o;
}

inline OuchCancelOrder make_cancel(uint64_t token_num, uint32_t shares = 100) {
    OuchCancelOrder c;
    std::memset(&c, 0, sizeof(c));
    c.msg_type = 'X';
    std::string token = std::to_string(token_num);
    token.insert(token.begin(), 14 - token.length(), '0');
    std::memcpy(c.order_token, token.c_str(), 14);
    c.shares = shares;
    return c;
}

} // namespace gwtest
