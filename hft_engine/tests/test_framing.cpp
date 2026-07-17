#include "tests.h"
#include "gateway/tcp_server.h"
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <memory>
#include <string>

// TCP framing and buffer compaction.
//
// OUCH messages are fixed-size but TCP is a byte stream: a read can deliver
// half a message, or two and a half. The gateway keeps a per-connection buffer
// and compacts the unparsed fragment to the front before each read. A previous
// bug hard-reset that buffer instead, discarding partial messages and desyncing
// the stream — these tests exist so that cannot come back silently.
//
// The gateway is driven over a socketpair rather than a real listener: no
// accept, no port races, and the test controls exactly how the bytes are split.

namespace {

// A test port that no part of the system uses; the constructor still binds,
// even though these tests never accept on the listener.
constexpr int TEST_PORT = 19091;

struct GatewayFixture {
    std::array<std::vector<std::unique_ptr<LockFreeQueue<EngineTask, 524288>>>, NUM_SHARDS> queues;
    std::vector<std::unique_ptr<LockFreeQueue<DropCopyMessage, 1048576>>> reject_queues;
    std::array<std::unique_ptr<MemoryPool<Order>>, NUM_SHARDS> pools;
    std::unique_ptr<TCPServer> server;
    std::unordered_map<int, TCPServer::ClientState> states;
    int client_fd = -1;   // test writes here
    int server_fd = -1;   // gateway reads here

    GatewayFixture() {
        // One gateway worker; every test order targets instrument 0 -> shard 0,
        // but all shards need a pool because the gateway indexes pools[shard].
        for (int s = 0; s < NUM_SHARDS; ++s) {
            pools[s] = std::make_unique<MemoryPool<Order>>(4096);
            queues[s].push_back(std::make_unique<LockFreeQueue<EngineTask, 524288>>());
        }
        reject_queues.push_back(std::make_unique<LockFreeQueue<DropCopyMessage, 1048576>>());

        server = std::make_unique<TCPServer>(TEST_PORT, 1, queues, reject_queues, pools);

        int fds[2];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
        client_fd = fds[0];
        server_fd = fds[1];

        // The gateway's read loop runs until EAGAIN, so its end must be
        // non-blocking or handle_client would never return.
        int flags = fcntl(server_fd, F_GETFL, 0);
        fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);

        states[server_fd];
    }

    ~GatewayFixture() {
        if (client_fd >= 0) close(client_fd);
        // handle_client closes server_fd itself on a framing error; only close
        // it here if it is still open.
        if (server_fd >= 0 && fcntl(server_fd, F_GETFD) != -1) close(server_fd);
        for (int fd : server->listen_fds) close(fd);
        for (int fd : server->epoll_fds) close(fd);
    }

    void pump() { server->handle_client(server_fd, states, 0); }

    void send_bytes(const void* p, size_t n) {
        CHECK(write(client_fd, p, n) == (ssize_t)n);
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
};

// A valid order for instrument 0 (STK00000), which routes to shard 0.
OuchEnterOrder make_order(uint64_t token_num, uint32_t price, uint32_t shares, char side) {
    OuchEnterOrder o;
    std::memset(&o, 0, sizeof(o));
    o.msg_type = 'O';
    std::string token = std::to_string(token_num);
    token.insert(token.begin(), 14 - token.length(), '0');
    std::memcpy(o.order_token, token.c_str(), 14);
    o.side = side;
    o.shares = shares;
    std::memcpy(o.stock, "STK00000", 8);
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

} // namespace

void test_framing() {
    // --- One whole message in one write. The baseline. ---
    {
        GatewayFixture f;
        OuchEnterOrder o = make_order(1, 50000, 100, 'B');
        f.send_bytes(&o, sizeof(o));
        f.pump();
        CHECK(f.tasks_enqueued() == 1);
    }

    // --- One message split across two reads. ---
    // The first pump sees 40 of 81 bytes and must enqueue nothing while
    // retaining the fragment; the second must complete it.
    {
        GatewayFixture f;
        OuchEnterOrder o = make_order(2, 50000, 100, 'B');
        const char* p = reinterpret_cast<const char*>(&o);

        f.send_bytes(p, 40);
        f.pump();
        CHECK(f.tasks_enqueued() == 0);   // incomplete: nothing enqueued yet

        f.send_bytes(p + 40, sizeof(o) - 40);
        f.pump();
        CHECK(f.tasks_enqueued() == 1);   // fragment was kept, not discarded
    }

    // --- Byte-at-a-time delivery: the pathological split. ---
    {
        GatewayFixture f;
        OuchEnterOrder o = make_order(3, 50000, 100, 'B');
        const char* p = reinterpret_cast<const char*>(&o);
        for (size_t i = 0; i < sizeof(o); ++i) {
            f.send_bytes(p + i, 1);
            f.pump();
            if (i < sizeof(o) - 1) CHECK(f.tasks_enqueued() == 0);
        }
        CHECK(f.tasks_enqueued() == 1);
    }

    // --- Two whole messages in one write. ---
    {
        GatewayFixture f;
        OuchEnterOrder a = make_order(4, 50000, 100, 'B');
        OuchEnterOrder b = make_order(5, 50001, 100, 'S');
        char buf[sizeof(a) * 2];
        std::memcpy(buf, &a, sizeof(a));
        std::memcpy(buf + sizeof(a), &b, sizeof(b));
        f.send_bytes(buf, sizeof(buf));
        f.pump();
        CHECK(f.tasks_enqueued() == 2);
    }

    // --- Compaction: a whole message plus a fragment of the next. ---
    // This is the exact shape the old hard-reset got wrong. After parsing the
    // first message, read_pos > 0 and a 40-byte fragment sits behind it; that
    // fragment must be memmove'd to the front and survive to the next read.
    {
        GatewayFixture f;
        OuchEnterOrder a = make_order(6, 50000, 100, 'B');
        OuchEnterOrder b = make_order(7, 50001, 100, 'S');

        char buf[sizeof(a) + 40];
        std::memcpy(buf, &a, sizeof(a));
        std::memcpy(buf + sizeof(a), &b, 40);
        f.send_bytes(buf, sizeof(buf));
        f.pump();
        CHECK(f.tasks_enqueued() == 1);   // only the complete one

        f.send_bytes(reinterpret_cast<const char*>(&b) + 40, sizeof(b) - 40);
        f.pump();
        CHECK(f.tasks_enqueued() == 1);   // the compacted fragment completed
    }

    // --- A long run of messages split at an offset that never aligns. ---
    // 81-byte messages delivered in 50-byte chunks: every message boundary
    // lands mid-chunk, so compaction runs on essentially every pass.
    {
        GatewayFixture f;
        constexpr int N = 20;
        char buf[sizeof(OuchEnterOrder) * N];
        for (int i = 0; i < N; ++i) {
            OuchEnterOrder o = make_order(100 + i, 50000, 100, (i % 2) ? 'S' : 'B');
            std::memcpy(buf + i * sizeof(o), &o, sizeof(o));
        }

        int total = 0;
        size_t sent = 0;
        while (sent < sizeof(buf)) {
            size_t chunk = std::min<size_t>(50, sizeof(buf) - sent);
            f.send_bytes(buf + sent, chunk);
            sent += chunk;
            f.pump();
            total += f.tasks_enqueued();
        }
        CHECK(total == N);   // every message decoded exactly once
    }

    // --- A cancel is 19 bytes, not 81: mixed sizes must stay in frame. ---
    {
        GatewayFixture f;
        OuchEnterOrder o = make_order(8, 50000, 100, 'B');
        f.send_bytes(&o, sizeof(o));
        f.pump();
        CHECK(f.tasks_enqueued() == 1);

        OuchCancelOrder c;
        std::memset(&c, 0, sizeof(c));
        c.msg_type = 'X';
        std::memcpy(c.order_token, "00000000000008", 14);
        c.shares = 100;

        OuchEnterOrder o2 = make_order(9, 50000, 100, 'B');
        char buf[sizeof(c) + sizeof(o2)];
        std::memcpy(buf, &c, sizeof(c));
        std::memcpy(buf + sizeof(c), &o2, sizeof(o2));
        f.send_bytes(buf, sizeof(buf));
        f.pump();
        // The cancel resolves against the live order from above, and the order
        // after it must still be framed correctly.
        CHECK(f.tasks_enqueued() == 2);
    }

    // --- An unknown message type drops the connection. ---
    // Framing cannot resync from an unknown type: the size is unknown, so
    // every following byte is suspect.
    {
        GatewayFixture f;
        char junk = 'Z';
        f.send_bytes(&junk, 1);
        f.pump();
        CHECK(f.tasks_enqueued() == 0);
        CHECK(f.states.find(f.server_fd) == f.states.end());  // connection dropped
        f.server_fd = -1;  // handle_client closed it
    }

    // --- A risk-rejected order must not desync the stream. ---
    // shares == 0 fails the pre-trade check; the message is still 81 bytes and
    // the order behind it must decode normally.
    {
        GatewayFixture f;
        OuchEnterOrder bad = make_order(10, 50000, 0, 'B');   // shares == 0 -> reject
        OuchEnterOrder good = make_order(11, 50000, 100, 'B');
        char buf[sizeof(bad) + sizeof(good)];
        std::memcpy(buf, &bad, sizeof(bad));
        std::memcpy(buf + sizeof(bad), &good, sizeof(good));
        f.send_bytes(buf, sizeof(buf));
        f.pump();
        CHECK(f.rejects_enqueued() == 1);
        CHECK(f.tasks_enqueued() == 1);
    }
}
