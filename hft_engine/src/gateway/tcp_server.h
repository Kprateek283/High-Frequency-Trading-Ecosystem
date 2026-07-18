#pragma once
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <iostream>
#include <cstring>
#include <vector>
#include <memory>
#include <unordered_map>
#include <sys/resource.h>
#include <netinet/tcp.h>
#include "core/lock_free_queue.h"
#include "core/memory_pool.h"
#include "matching/order.h"
#include "core/timer.h"
#include "gateway/risk_engine.h"
#include "gateway/session_manager.h"

constexpr int NUM_SHARDS = 4;

class TCPServer {
public:
    int port;
    int num_threads;
    std::vector<int> listen_fds;
    std::vector<int> epoll_fds;
    std::vector<std::thread> workers;

    // engine_qs[shard][worker] is the SPSC ingress queue owned by gateway worker
    // `worker` into engine shard `shard`. reject_qs[worker] is that worker's
    // SPSC reject-report queue (drained by the OrderManager). One producer per
    // queue keeps LockFreeQueue's SPSC contract intact under a multi-threaded gateway.
    TCPServer(int port, int threads,
              std::array<std::vector<std::unique_ptr<LockFreeQueue<EngineTask, 524288>>>, NUM_SHARDS>& qs,
              std::vector<std::unique_ptr<LockFreeQueue<DropCopyMessage, 1048576>>>& reject_qs,
              std::array<std::unique_ptr<MemoryPool<Order>>, NUM_SHARDS>& ps)
        : port(port), queues(qs), gw_reject_queues(reject_qs), pools(ps) {

        num_threads = threads;
        if (num_threads < 1) num_threads = 1;
        if (num_threads > 16) num_threads = 16;

        // Raise the open-fd soft limit toward the hard limit so the gateway isn't
        // capped near the default ~1024 concurrent connections. Per-connection state
        // is now created lazily on accept (see worker_loop), so there is no fixed cap.
        struct rlimit rl;
        if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
            rl.rlim_cur = rl.rlim_max;
            setrlimit(RLIMIT_NOFILE, &rl);
        }

        for (int i = 0; i < num_threads; ++i) {
            int l_fd = socket(AF_INET, SOCK_STREAM, 0);
            int opt = 1;
            setsockopt(l_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            setsockopt(l_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)); // CRITICAL for sharding
            setsockopt(l_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
            
            int busy_poll = 50; 
            setsockopt(l_fd, SOL_SOCKET, SO_BUSY_POLL, &busy_poll, sizeof(busy_poll));
            
            struct sockaddr_in addr;
            std::memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port = htons(port);
            
            if (bind(l_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                throw std::runtime_error("Failed to bind TCP to port " + std::to_string(port));
            }
            if (listen(l_fd, 10) < 0) {
                throw std::runtime_error("Failed to listen on TCP socket.");
            }
            listen_fds.push_back(l_fd);

            int e_fd = epoll_create1(0);
            struct epoll_event ev;
            ev.events = EPOLLIN;
            ev.data.fd = l_fd;
            epoll_ctl(e_fd, EPOLL_CTL_ADD, l_fd, &ev);

            epoll_fds.push_back(e_fd);
        }
    }

    std::atomic<uint64_t> total_udp{0};
    std::atomic<uint64_t> total_trading{0};
    std::atomic<uint64_t> total_q{0};
    std::atomic<uint64_t> total_tcp{0};
    std::atomic<uint64_t> total_e2e{0};
    std::atomic<uint64_t> num_samples{0};

    // Gateway Attribution Metrics
    std::atomic<uint64_t> total_epoll_cycles{0};
    std::atomic<uint64_t> total_read_cycles{0};
    std::atomic<uint64_t> total_decode_cycles{0};
    std::atomic<uint64_t> total_validation_cycles{0};
    std::atomic<uint64_t> total_enqueue_cycles{0};
    std::atomic<uint64_t> total_orders_processed{0};

    void print_experiment_4_stats() {
        uint64_t n = num_samples.load();
        if (n > 0) {
            std::cout << "\n========================================\n";
            std::cout << " EXPERIMENT 4: Latency Decomposition \n";
            std::cout << "========================================\n";
            std::cout << "Samples       : " << n << "\n";
            std::cout << "UDP Path      : " << (total_udp.load() / n) << " cycles\n";
            std::cout << "Trading Engine: " << (total_trading.load() / n) << " cycles\n";
            std::cout << "SPSC Queue    : " << (total_q.load() / n) << " cycles\n";
            std::cout << "TCP Path      : " << (total_tcp.load() / n) << " cycles\n";
            std::cout << "----------------------------------------\n";
            std::cout << "End-to-End    : " << (total_e2e.load() / n) << " cycles\n";
            std::cout << "========================================\n";
        }
        
        uint64_t orders = total_orders_processed.load();
        if (orders > 0) {
            std::cout << "\n========================================\n";
            std::cout << " Exchange Gateway Attribution (Average) \n";
            std::cout << "========================================\n";
            std::cout << "epoll_wait() : " << (total_epoll_cycles.load() / orders) << " cycles/order\n";
            std::cout << "read()       : " << (total_read_cycles.load() / orders) << " cycles/order\n";
            std::cout << "Decode       : " << (total_decode_cycles.load() / orders) << " cycles/order\n";
            std::cout << "Validation   : " << (total_validation_cycles.load() / orders) << " cycles/order\n";
            std::cout << "Enqueue      : " << (total_enqueue_cycles.load() / orders) << " cycles/order\n";
            std::cout << "Total/Order  : " << ((total_epoll_cycles.load() + total_read_cycles.load() + total_decode_cycles.load() + total_validation_cycles.load() + total_enqueue_cycles.load()) / orders) << " cycles/order\n";
            std::cout << "========================================\n";
        }
    }

    void worker_loop(int thread_id, std::atomic<bool>& running) {
        int e_fd = epoll_fds[thread_id];
        int l_fd = listen_fds[thread_id];

        // Per-worker connection table keyed by raw fd. No fixed cap (removes the old
        // 1024-fd limit) and no giant up-front allocation — state is created on accept
        // and destroyed on close. Owned by this worker, so no cross-thread sharing.
        std::unordered_map<int, ClientState> client_states;

        struct epoll_event events[64];
        while (running.load(std::memory_order_relaxed)) {
            unsigned aux;
            uint64_t e1 = __rdtscp(&aux);
            int nfds = epoll_wait(e_fd, events, 64, 100);
            uint64_t e2 = __rdtscp(&aux);
            if (nfds > 0) {
                total_epoll_cycles.fetch_add(e2 - e1, std::memory_order_relaxed);
            }
            
            for (int i = 0; i < nfds; ++i) {
                if (events[i].data.fd == l_fd) {
                    int client_fd = accept(l_fd, NULL, NULL);
                    if (client_fd < 0) continue;

                    int flags = fcntl(client_fd, F_GETFL, 0);
                    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

                    int nodelay = 1;
                    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
                    int busy_poll = 50;
                    setsockopt(client_fd, SOL_SOCKET, SO_BUSY_POLL, &busy_poll, sizeof(busy_poll));

                    // Identity is assigned here, from the connection, and never
                    // from a client-controlled field. Ids start at 1 so that 0
                    // stays available as "no client" in the session map.
                    client_states[client_fd].client_id =
                        next_client_id.fetch_add(1, std::memory_order_relaxed);

                    struct epoll_event ev;
                    ev.events = EPOLLIN | EPOLLET;
                    ev.data.fd = client_fd;
                    epoll_ctl(e_fd, EPOLL_CTL_ADD, client_fd, &ev);
                } else {
                    handle_client(events[i].data.fd, client_states, thread_id);
                }
            }
        }
    }

    void run(std::atomic<bool>& running) {
        for (int i = 0; i < num_threads; ++i) {
            workers.emplace_back([this, i, &running]() {
                pthread_setname_np(pthread_self(), ("Gateway-" + std::to_string(i)).c_str());
                this->worker_loop(i, running);
            });
        }
        for (auto& t : workers) {
            if (t.joinable()) t.join();
        }
    }

public:
    struct ClientState {
        // Only ever holds at most one partial message plus a fresh read, so this is
        // far larger than needed; kept generous for big batched reads. Allocated per
        // live connection, so there is no fixed connection cap.
        char buffer[16384];
        size_t write_pos = 0;
        size_t read_pos = 0;
        // This connection's identity, assigned by the gateway on accept. Never
        // derived from anything the client sends (review A6).
        uint32_t client_id = 0;
    };

    // Public only so the framing tests can drive it over a socketpair without a
    // live listener; in production only worker_loop calls it.
    void handle_client(int fd, std::unordered_map<int, ClientState>& states, int worker_id) {
        auto it = states.find(fd);
        if (it == states.end()) return;
        ClientState& state = it->second;

        while (true) {
            // Reclaim buffer space before each read: reset when fully consumed,
            // otherwise compact the unparsed fragment to the front (never discard it,
            // which the old hard-reset did and thereby desynced the framing).
            if (state.read_pos == state.write_pos) {
                state.read_pos = state.write_pos = 0;
            } else if (state.read_pos > 0) {
                size_t frag = state.write_pos - state.read_pos;
                std::memmove(state.buffer, state.buffer + state.read_pos, frag);
                state.write_pos = frag;
                state.read_pos = 0;
            }
            size_t remaining_space = sizeof(state.buffer) - state.write_pos;
            if (remaining_space == 0) {
                // A single unparsed fragment fills the whole buffer => malformed /
                // oversized framing. Drop the connection rather than corrupt the stream.
                close(fd);
                states.erase(fd);
                return;
            }

            unsigned aux;
            uint64_t r1 = __rdtscp(&aux);
            ssize_t n = read(fd, state.buffer + state.write_pos, remaining_space);
            uint64_t r2 = __rdtscp(&aux);
            uint64_t t5 = r2; // using r2 as t5

            if (n > 0) {
                total_read_cycles.fetch_add(r2 - r1, std::memory_order_relaxed);
                state.write_pos += n;

                while (state.write_pos - state.read_pos >= 1) {
                    uint64_t d1 = __rdtscp(&aux);
                    char msg_type = state.buffer[state.read_pos];
                    size_t msg_size = 0;
                    if (msg_type == 'O') msg_size = sizeof(OuchEnterOrder);
                    else if (msg_type == 'X') msg_size = sizeof(OuchCancelOrder);
                    else {
                        close(fd);
                        states.erase(fd);
                        return;
                    }

                    if (state.write_pos - state.read_pos < msg_size) break;

                    if (msg_type == 'O') {
                        OuchEnterOrder req;
                        std::memcpy(&req, state.buffer + state.read_pos, sizeof(OuchEnterOrder));
                        
                        if (req.t1_exchange_send != 0 && req.t2_trading_recv != 0 && req.t3_trading_enq != 0 && req.t4_network_deq != 0) {
                            total_udp += (req.t2_trading_recv - req.t1_exchange_send);
                            total_trading += (req.t3_trading_enq - req.t2_trading_recv);
                            total_q += (req.t4_network_deq - req.t3_trading_enq);
                            total_tcp += (t5 - req.t4_network_deq);
                            total_e2e += (t5 - req.t1_exchange_send);
                            num_samples++;
                        }
                        
                        // The token identifies the order; the connection
                        // identifies the client. Never the other way round (A6).
                        uint64_t order_token = decode_order_token(req.order_token);
                        const uint32_t client_id = state.client_id;

                        uint16_t inst = decode_symbol(req.stock);

                        Side side = (req.side == 'B') ? Side::BUY : Side::SELL;
                        uint64_t d2 = __rdtscp(&aux);

                        // A token that is not 14 digits, or that falls outside the
                        // session map, cannot be registered — so its cancel could
                        // never resolve. Reject it rather than accept an order the
                        // client can never cancel.
                        bool safe = order_token < SessionManager::MAX_CLIENT_ORDERS
                                    && risk_engine.check_pre_trade(req, inst);
                        uint64_t v1 = __rdtscp(&aux);

                        if (!safe) {
                            // Rejected orders never enter the book (no slot handle, id 0).
                            // A malformed token reports back as 0; there is no
                            // meaningful order id to echo.
                            uint64_t echo = (order_token == INVALID_ORDER_TOKEN) ? 0 : order_token;
                            DropCopyMessage drop_msg = {echo, 0, req.price, req.shares, inst, OrderState::REJECTED, side};
                            gw_reject_queues[worker_id]->push(drop_msg);
                            state.read_pos += sizeof(OuchEnterOrder);
                            uint64_t e1 = __rdtscp(&aux);
                            total_decode_cycles.fetch_add(d2 - d1, std::memory_order_relaxed);
                            total_validation_cycles.fetch_add(v1 - d2, std::memory_order_relaxed);
                            total_enqueue_cycles.fetch_add(e1 - v1, std::memory_order_relaxed);
                            total_orders_processed.fetch_add(1, std::memory_order_relaxed);
                            continue;
                        }

                        uint16_t shard = inst % NUM_SHARDS;
                        Order* o = pools[shard]->allocate(0, order_token, client_id, req.price, req.shares, inst, side);
                        if (!o) [[unlikely]] {
                            // Pool exhausted: too many live orders in this shard. The
                            // order never enters the book; reject it like a risk reject
                            // and keep serving instead of tearing down the process.
                            DropCopyMessage drop_msg = {order_token, 0, req.price, req.shares, inst, OrderState::REJECTED, side};
                            gw_reject_queues[worker_id]->push(drop_msg);
                            state.read_pos += sizeof(OuchEnterOrder);
                            total_orders_processed.fetch_add(1, std::memory_order_relaxed);
                            continue;
                        }
                        uint32_t internal_id = pools[shard]->index_of(o); // pool slot IS the handle
                        o->internal_id = internal_id;
                        session_manager.record_order(order_token, internal_id, inst, client_id);
                        EngineTask task;
                        task.type = MsgType::NEW;
                        task.order = o;
                        task.ingress_tsc = get_tsc();
                        while (!queues[shard][worker_id]->push(task)) { __builtin_ia32_pause(); }
                        state.read_pos += sizeof(OuchEnterOrder);

                        uint64_t e1 = __rdtscp(&aux);
                        total_decode_cycles.fetch_add(d2 - d1, std::memory_order_relaxed);
                        total_validation_cycles.fetch_add(v1 - d2, std::memory_order_relaxed);
                        total_enqueue_cycles.fetch_add(e1 - v1, std::memory_order_relaxed);
                        total_orders_processed.fetch_add(1, std::memory_order_relaxed);
                    } else if (msg_type == 'X') {
                        uint64_t d1 = __rdtscp(&aux);
                        OuchCancelOrder req;
                        std::memcpy(&req, state.buffer + state.read_pos, sizeof(OuchCancelOrder));
                        
                        uint64_t order_token = decode_order_token(req.order_token);

                        OrderSessionData data =
                            (order_token < SessionManager::MAX_CLIENT_ORDERS)
                                ? session_manager.lookup_data(order_token)
                                : OrderSessionData{0, 0, 0};
                        uint64_t d2 = __rdtscp(&aux);

                        // Only the connection that owns the order may cancel it.
                        // Identity comes from the session on both sides of this
                        // comparison, so a client cannot cancel another's order by
                        // guessing its token (review A6).
                        if (data.internal_id != 0 && data.client_id == state.client_id) [[likely]] {
                            uint16_t shard = data.instrument_id % NUM_SHARDS;
                            EngineTask task;
                            task.type = MsgType::CANCEL;
                            task.cancel.internal_id = data.internal_id;
                            task.cancel.client_order_id = order_token;
                            task.ingress_tsc = get_tsc();
                            while (!queues[shard][worker_id]->push(task)) { __builtin_ia32_pause(); }
                        }
                        state.read_pos += sizeof(OuchCancelOrder);
                        uint64_t e1 = __rdtscp(&aux);
                        total_decode_cycles.fetch_add(d2 - d1, std::memory_order_relaxed);
                        total_enqueue_cycles.fetch_add(e1 - d2, std::memory_order_relaxed);
                        total_orders_processed.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                
                // Buffer reclamation happens at the top of the next loop iteration.
            } else if (n == 0) {
                close(fd);
                states.erase(fd);
                return;
            } else {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    close(fd);
                    states.erase(fd);
                }
                return; // EAGAIN: socket drained, keep the connection for next event
            }
        }
    }

private:
    // Server-assigned connection identity. Shared across workers because two
    // SO_REUSEPORT workers can accept concurrently and ids must stay unique.
    // Starts at 1: 0 means "no client" in the session map.
    std::atomic<uint32_t> next_client_id{1};

    std::array<std::vector<std::unique_ptr<LockFreeQueue<EngineTask, 524288>>>, NUM_SHARDS>& queues;
    std::vector<std::unique_ptr<LockFreeQueue<DropCopyMessage, 1048576>>>& gw_reject_queues;
    std::array<std::unique_ptr<MemoryPool<Order>>, NUM_SHARDS>& pools;
    RiskEngine risk_engine;
    SessionManager session_manager;
};
