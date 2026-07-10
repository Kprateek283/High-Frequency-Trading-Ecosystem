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
#include <netinet/tcp.h>
#include "core/lock_free_queue.h"
#include "core/memory_pool.h"
#include "matching/order.h"
#include "core/timer.h"
#include "gateway/risk_engine.h"
#include "gateway/session_manager.h"

const int MAX_FDS = 1024;
constexpr int NUM_SHARDS = 4;

class TCPServer {
public:
    int port;
    int num_threads;
    std::vector<int> listen_fds;
    std::vector<int> udp_fds;
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

        clients = std::make_unique<ClientState[]>(MAX_FDS * num_threads);

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

            int u_fd = socket(AF_INET, SOCK_DGRAM, 0);
            setsockopt(u_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            setsockopt(u_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)); // CRITICAL for sharding
            setsockopt(u_fd, SOL_SOCKET, SO_BUSY_POLL, &busy_poll, sizeof(busy_poll));
            int udp_flags = fcntl(u_fd, F_GETFL, 0);
            fcntl(u_fd, F_SETFL, udp_flags | O_NONBLOCK);

            struct sockaddr_in udp_addr;
            std::memset(&udp_addr, 0, sizeof(udp_addr));
            udp_addr.sin_family = AF_INET;
            udp_addr.sin_addr.s_addr = INADDR_ANY;
            udp_addr.sin_port = htons(port + 1); // 9092
            
            if (bind(u_fd, (struct sockaddr*)&udp_addr, sizeof(udp_addr)) < 0) {
                throw std::runtime_error("Failed to bind UDP to port " + std::to_string(port + 1));
            }
            udp_fds.push_back(u_fd);

            int e_fd = epoll_create1(0);
            struct epoll_event ev;
            ev.events = EPOLLIN;
            ev.data.fd = l_fd;
            epoll_ctl(e_fd, EPOLL_CTL_ADD, l_fd, &ev);
            
            struct epoll_event ev_udp;
            ev_udp.events = EPOLLIN | EPOLLET;
            ev_udp.data.fd = u_fd;
            epoll_ctl(e_fd, EPOLL_CTL_ADD, u_fd, &ev_udp);
            
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
        int u_fd = udp_fds[thread_id];
        int fd_offset = thread_id * MAX_FDS; // Ensure unique indexes per thread for ClientState
        
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
                    if (client_fd < 0 || client_fd >= MAX_FDS) {
                        if (client_fd >= 0) close(client_fd);
                        continue;
                    }
                    
                    int flags = fcntl(client_fd, F_GETFL, 0);
                    fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
                    
                    int nodelay = 1;
                    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
                    int busy_poll = 50;
                    setsockopt(client_fd, SOL_SOCKET, SO_BUSY_POLL, &busy_poll, sizeof(busy_poll));
                    
                    struct epoll_event ev;
                    ev.events = EPOLLIN | EPOLLET;
                    // Tag the fd with offset to map into global clients array safely
                    ev.data.fd = client_fd + fd_offset; 
                    epoll_ctl(e_fd, EPOLL_CTL_ADD, client_fd, &ev);
                } else if (events[i].data.fd == u_fd) {
                    handle_udp_client(u_fd, thread_id);
                } else {
                    handle_client(events[i].data.fd, events[i].data.fd - fd_offset, thread_id);
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

private:
    void process_message(const char* buf, size_t len, uint64_t t5, int worker_id) {
        char msg_type = buf[0];
        if (msg_type == 'O') {
            if (len < sizeof(OuchEnterOrder)) return;
            OuchEnterOrder req;
            std::memcpy(&req, buf, sizeof(OuchEnterOrder));
            
            if (req.t1_exchange_send != 0 && req.t2_trading_recv != 0 && req.t3_trading_enq != 0 && req.t4_network_deq != 0) {
                total_udp += (req.t2_trading_recv - req.t1_exchange_send);
                total_trading += (req.t3_trading_enq - req.t2_trading_recv);
                total_q += (req.t4_network_deq - req.t3_trading_enq);
                total_tcp += (t5 - req.t4_network_deq);
                total_e2e += (t5 - req.t1_exchange_send);
                num_samples++;
            }
            
            uint64_t client_id = 0;
            for(int j=0; j<14; ++j) {
                if(req.order_token[j] >= '0' && req.order_token[j] <= '9') {
                    client_id = client_id * 10 + (req.order_token[j] - '0');
                }
            }

            uint16_t inst = 999;
            if (req.stock[0] == 'S' && req.stock[1] == 'T' && req.stock[2] == 'K') {
                inst = (req.stock[3] - '0') * 10000 +
                       (req.stock[4] - '0') * 1000 +
                       (req.stock[5] - '0') * 100 +
                       (req.stock[6] - '0') * 10 +
                       (req.stock[7] - '0');
            }
            
            Side side = (req.side == 'B') ? Side::BUY : Side::SELL;

            if (!risk_engine.check_pre_trade(req, inst)) {
                // Rejected orders never enter the book, so they get no slot handle (id 0).
                DropCopyMessage drop_msg = {client_id, 0, req.price, req.shares, inst, OrderState::REJECTED, side};
                gw_reject_queues[worker_id]->push(drop_msg);
                return;
            }

            uint16_t shard = inst % NUM_SHARDS;
            Order* o = pools[shard]->allocate(0, client_id, req.price, req.shares, inst, side);
            uint32_t internal_id = pools[shard]->index_of(o); // pool slot IS the handle
            o->internal_id = internal_id;
            session_manager.record_order(client_id, internal_id, inst);
            EngineTask task;
            task.type = MsgType::NEW;
            task.order = o;
            task.ingress_tsc = get_tsc();
            while (!queues[shard][worker_id]->push(task)) { __builtin_ia32_pause(); }
        } else if (msg_type == 'X') {
            if (len < sizeof(OuchCancelOrder)) return;
            OuchCancelOrder req;
            std::memcpy(&req, buf, sizeof(OuchCancelOrder));

            uint64_t client_id = 0;
            for(int j=0; j<14; ++j) {
                if(req.order_token[j] >= '0' && req.order_token[j] <= '9') {
                    client_id = client_id * 10 + (req.order_token[j] - '0');
                }
            }

            OrderSessionData data = session_manager.lookup_data(client_id);
            if (data.internal_id != 0) [[likely]] {
                uint16_t shard = data.instrument_id % NUM_SHARDS;
                EngineTask task;
                task.type = MsgType::CANCEL;
                task.cancel.internal_id = data.internal_id;
                task.cancel.client_order_id = client_id;
                task.ingress_tsc = get_tsc();
                while (!queues[shard][worker_id]->push(task)) { __builtin_ia32_pause(); }
            }
        }
    }

    void handle_udp_client(int udp_fd, int worker_id) {
        char buf[2048];
        while (true) {
            ssize_t n = recv(udp_fd, buf, sizeof(buf), MSG_DONTWAIT);
            if (n <= 0) break;
            unsigned aux;
            uint64_t t5 = __rdtscp(&aux);
            process_message(buf, n, t5, worker_id);
        }
    }
    struct ClientState {
        char buffer[131072]; 
        size_t write_pos = 0;
        size_t read_pos = 0;
    };
    
    std::unique_ptr<ClientState[]> clients;

    void handle_client(int global_fd, int local_fd, int worker_id) {
        auto& state = clients[global_fd];
        
        while (true) {
            size_t remaining_space = sizeof(state.buffer) - state.write_pos;
            if (remaining_space == 0) {
                state.read_pos = 0;
                state.write_pos = 0;
                remaining_space = sizeof(state.buffer);
            }

            unsigned aux;
            uint64_t r1 = __rdtscp(&aux);
            ssize_t n = read(local_fd, state.buffer + state.write_pos, remaining_space);
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
                        close(local_fd);
                        std::memset(static_cast<void*>(&state), 0, sizeof(ClientState));
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
                        
                        uint64_t client_id = 0;
                        for(int j=0; j<14; ++j) {
                            if(req.order_token[j] >= '0' && req.order_token[j] <= '9') {
                                client_id = client_id * 10 + (req.order_token[j] - '0');
                            }
                        }

                        uint16_t inst = 999;
                        if (req.stock[0] == 'S' && req.stock[1] == 'T' && req.stock[2] == 'K') {
                            inst = (req.stock[3] - '0') * 10000 +
                                   (req.stock[4] - '0') * 1000 +
                                   (req.stock[5] - '0') * 100 +
                                   (req.stock[6] - '0') * 10 +
                                   (req.stock[7] - '0');
                        }
                        
                        Side side = (req.side == 'B') ? Side::BUY : Side::SELL;
                        uint64_t d2 = __rdtscp(&aux);

                        bool safe = risk_engine.check_pre_trade(req, inst);
                        uint64_t v1 = __rdtscp(&aux);

                        if (!safe) {
                            // Rejected orders never enter the book (no slot handle, id 0).
                            DropCopyMessage drop_msg = {client_id, 0, req.price, req.shares, inst, OrderState::REJECTED, side};
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
                        Order* o = pools[shard]->allocate(0, client_id, req.price, req.shares, inst, side);
                        uint32_t internal_id = pools[shard]->index_of(o); // pool slot IS the handle
                        o->internal_id = internal_id;
                        session_manager.record_order(client_id, internal_id, inst);
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
                        
                        uint64_t client_id = 0;
                        for(int j=0; j<14; ++j) {
                            if(req.order_token[j] >= '0' && req.order_token[j] <= '9') {
                                client_id = client_id * 10 + (req.order_token[j] - '0');
                            }
                        }

                        OrderSessionData data = session_manager.lookup_data(client_id);
                        uint64_t d2 = __rdtscp(&aux);
                        
                        if (data.internal_id != 0) [[likely]] {
                            uint16_t shard = data.instrument_id % NUM_SHARDS;
                            EngineTask task;
                            task.type = MsgType::CANCEL;
                            task.cancel.internal_id = data.internal_id;
                            task.cancel.client_order_id = client_id;
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
                
                if (state.read_pos == state.write_pos) {
                    state.read_pos = 0;
                    state.write_pos = 0;
                } else if (state.write_pos > sizeof(state.buffer) - 1024) {
                    size_t fragment_size = state.write_pos - state.read_pos;
                    std::memmove(state.buffer, state.buffer + state.read_pos, fragment_size);
                    state.read_pos = 0;
                    state.write_pos = fragment_size;
                }
            } else if (n == 0) {
                close(local_fd);
                std::memset(static_cast<void*>(&state), 0, sizeof(ClientState));
                break;
            } else {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    close(local_fd);
                    std::memset(static_cast<void*>(&state), 0, sizeof(ClientState));
                }
                break;
            }
        }
    }

    std::array<std::vector<std::unique_ptr<LockFreeQueue<EngineTask, 524288>>>, NUM_SHARDS>& queues;
    std::vector<std::unique_ptr<LockFreeQueue<DropCopyMessage, 1048576>>>& gw_reject_queues;
    std::array<std::unique_ptr<MemoryPool<Order>>, NUM_SHARDS>& pools;
    RiskEngine risk_engine;
    SessionManager session_manager;
    
    // We parse "AAPL    " as a 64-bit uint64_t for instantaneous symbol matching
    const uint64_t supported_stocks[4] = {
        *reinterpret_cast<const uint64_t*>("AAPL    "),
        *reinterpret_cast<const uint64_t*>("MSFT    "),
        *reinterpret_cast<const uint64_t*>("GOOG    "),
        *reinterpret_cast<const uint64_t*>("AMZN    ")
    };
};
