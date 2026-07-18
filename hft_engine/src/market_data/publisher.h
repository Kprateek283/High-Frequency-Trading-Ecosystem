#pragma once
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include "core/lock_free_queue.h"
#include "protocol/messages.h"

#include <array>
#include <memory>

class Publisher {
private:
    std::array<std::unique_ptr<LockFreeQueue<ItchMessage, 1048576>>, 4>& queues;
    std::atomic<bool>& running;

public:
    Publisher(std::array<std::unique_ptr<LockFreeQueue<ItchMessage, 1048576>>, 4>& qs, std::atomic<bool>& r) : queues(qs), running(r) {}

    void run() {
        pthread_setname_np(pthread_self(), "MarketData");
        
        int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);

        // Multicast group/port from config (defaults match the historical values).
        const char* group = std::getenv("MULTICAST_GROUP");
        const char* port = std::getenv("MULTICAST_PORT");
        struct sockaddr_in mcast_addr;
        std::memset(&mcast_addr, 0, sizeof(mcast_addr));
        mcast_addr.sin_family = AF_INET;
        mcast_addr.sin_addr.s_addr = inet_addr(group ? group : "239.255.0.1");
        mcast_addr.sin_port = htons(port ? static_cast<uint16_t>(std::atoi(port)) : 12345);

        // One-time multicast hardening (prep §2): make the send interface, TTL and
        // loopback explicit instead of relying on Linux defaults, so a monitor on
        // another host/interface can subscribe reliably. Off the hot path.
        int ttl = 1;
        setsockopt(udp_fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
        int loop = 1;
        setsockopt(udp_fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
        struct in_addr iface{};
        iface.s_addr = htonl(INADDR_ANY);
        setsockopt(udp_fd, IPPROTO_IP, IP_MULTICAST_IF, &iface, sizeof(iface));

        const size_t BATCH_SIZE = 64; 
        ItchMessage batch[BATCH_SIZE];
        struct iovec iov[BATCH_SIZE];
        struct mmsghdr msgs[BATCH_SIZE];
        
        for (size_t i = 0; i < BATCH_SIZE; ++i) {
            iov[i].iov_base = &batch[i];
            iov[i].iov_len = sizeof(ItchMessage);
            std::memset(&msgs[i], 0, sizeof(struct mmsghdr));
            msgs[i].msg_hdr.msg_name = &mcast_addr;
            msgs[i].msg_hdr.msg_namelen = sizeof(mcast_addr);
            msgs[i].msg_hdr.msg_iov = &iov[i];
            msgs[i].msg_hdr.msg_iovlen = 1;
        }

        size_t current_batch_count = 0;

        while (running.load(std::memory_order_relaxed)) {
            bool found_any = false;
            for (int i = 0; i < 4; ++i) {
                ItchMessage report;
                while (queues[i]->pop(report)) {
                    unsigned aux;
                    report.timestamp = __rdtscp(&aux);
                    batch[current_batch_count++] = report;
                    if (current_batch_count == BATCH_SIZE) {
                        sendmmsg(udp_fd, msgs, current_batch_count, 0);
                        current_batch_count = 0;
                    }
                    found_any = true;
                }
            }
            if (!found_any) {
                if (current_batch_count > 0) {
                    sendmmsg(udp_fd, msgs, current_batch_count, 0);
                    current_batch_count = 0;
                }
                __builtin_ia32_pause();
            }
        }
    }
};
