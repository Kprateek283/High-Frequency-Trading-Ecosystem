// Replay a generated order file at the gateway.
//
// Orders are spread over several TCP connections, which matters for two
// independent reasons:
//
//   1. Self-trade prevention rejects an order that would cross the same client's
//      resting order, and client identity is assigned per connection. Replaying
//      a crossing workload down ONE socket therefore cannot produce a single
//      fill -- every cross is a self-cross. A 5M-order file used to yield 0
//      matches and 3,000,004 rejects, i.e. it measured the reject path only.
//   2. SO_REUSEPORT distributes accepted CONNECTIONS across gateway workers, so
//      a single-socket client drives exactly one worker no matter how many are
//      configured.
//
// Orders are dealt round-robin so that consecutive orders -- the ones most
// likely to cross each other -- land on different connections and can trade.

#include <iostream>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "protocol/messages.h"

namespace {

int connect_gateway(const char* ip, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, ip, &addr.sin_addr);
    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    int sndbuf = 8 * 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    return sock;
}

// send() may write less than asked; a short write mid-message would desynchronise
// the gateway's framing for everything that follows on this connection.
bool send_all(int sock, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(sock, data + sent, len - sent, 0);
        if (n > 0) {
            sent += static_cast<size_t>(n);
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            __builtin_ia32_pause();
        } else {
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <orders.bin> [ip=127.0.0.1] [port=9091]\n"
                  << "  env: REPLAY_CONNECTIONS (default 4) — orders are dealt\n"
                  << "       round-robin across this many client connections.\n";
        return 1;
    }

    const char* file_path = argv[1];
    const char* ip = (argc >= 3) ? argv[2] : "127.0.0.1";
    int port = (argc >= 4) ? std::stoi(argv[3]) : 9091;

    const char* conn_env = std::getenv("REPLAY_CONNECTIONS");
    int num_conns = conn_env ? std::atoi(conn_env) : 4;
    if (num_conns < 1) num_conns = 1;

    int fd = open(file_path, O_RDONLY);
    if (fd < 0) {
        std::cerr << "Failed to open " << file_path << "\n";
        return 1;
    }

    struct stat sb;
    if (fstat(fd, &sb) != 0) {
        std::cerr << "Failed to stat " << file_path << "\n";
        close(fd);
        return 1;
    }
    const size_t file_size = static_cast<size_t>(sb.st_size);

    // The file is a flat OuchEnterOrder[] (no framing) — see docs/dependency.md.
    // Splitting it across connections means dealing whole orders, not the byte
    // chunks a single stream could get away with.
    const size_t order_size = sizeof(OuchEnterOrder);
    const size_t num_orders = file_size / order_size;
    if (num_orders == 0) {
        std::cerr << "File holds no complete orders (" << file_size << " bytes, need "
                  << order_size << " per order)\n";
        close(fd);
        return 1;
    }
    if (file_size % order_size != 0) {
        std::cerr << "Warning: trailing " << (file_size % order_size)
                  << " bytes are not a whole order; ignoring them.\n";
    }

    char* data = static_cast<char*>(mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0));
    if (data == MAP_FAILED) {
        std::cerr << "mmap failed\n";
        close(fd);
        return 1;
    }

    std::vector<int> socks;
    for (int i = 0; i < num_conns; ++i) {
        int s = connect_gateway(ip, port);
        if (s < 0) {
            std::cerr << "Failed to connect to engine at " << ip << ":" << port
                      << " (connection " << i << ")\n";
            for (int open_sock : socks) close(open_sock);
            munmap(data, file_size);
            close(fd);
            return 1;
        }
        socks.push_back(s);
    }

    std::cout << "Replay connected to " << ip << ":" << port << " on " << num_conns
              << " connection(s). Replaying " << num_orders << " orders ("
              << file_size << " bytes).\n";

    // Batch per connection so we are not paying a syscall per 81-byte order.
    const size_t ORDERS_PER_BATCH = 512;
    std::vector<std::vector<char>> batches(static_cast<size_t>(num_conns));
    for (auto& b : batches) b.reserve(ORDERS_PER_BATCH * order_size);

    auto start = std::chrono::high_resolution_clock::now();
    bool ok = true;

    for (size_t i = 0; i < num_orders && ok; ++i) {
        const size_t conn = i % static_cast<size_t>(num_conns);
        const char* order = data + i * order_size;
        batches[conn].insert(batches[conn].end(), order, order + order_size);

        if (batches[conn].size() >= ORDERS_PER_BATCH * order_size) {
            ok = send_all(socks[conn], batches[conn].data(), batches[conn].size());
            batches[conn].clear();
            // Pace injection so the gateway is not simply drowned in backlog.
            std::this_thread::sleep_for(std::chrono::microseconds(5));
        }
    }

    for (size_t c = 0; c < batches.size() && ok; ++c) {
        if (!batches[c].empty()) {
            ok = send_all(socks[c], batches[c].data(), batches[c].size());
        }
    }

    if (!ok) std::cerr << "Socket error during replay\n";

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;

    std::cout << "Replay Complete!\n";
    std::cout << "Time elapsed: " << diff.count() << " seconds.\n";
    std::cout << "Throughput: " << (file_size / 1024.0 / 1024.0 / diff.count()) << " MB/sec\n";

    // Let the engine drain before the connections drop, so trailing orders are
    // matched rather than lost with the socket.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    for (int s : socks) close(s);
    munmap(data, file_size);
    close(fd);
    return ok ? 0 : 1;
}
