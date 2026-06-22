#pragma once
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>
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
        struct sockaddr_in mcast_addr;
        std::memset(&mcast_addr, 0, sizeof(mcast_addr));
        mcast_addr.sin_family = AF_INET;
        mcast_addr.sin_addr.s_addr = inet_addr("239.255.0.1");
        mcast_addr.sin_port = htons(12345);

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
