#pragma once
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <stdexcept>
#include <functional>
#include <iostream>

class UDPListener {
private:
    int sock_fd;
    int epoll_fd;

public:
    UDPListener(const char* ip, int port) {
        sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock_fd < 0) {
            throw std::runtime_error("Failed to create UDP socket");
        }

        // Allow multiple listeners on the same machine
        int opt = 1;
        setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        // Enable low-latency busy polling at the driver level (if supported)
        int busy_poll = 50; 
        setsockopt(sock_fd, SOL_SOCKET, SO_BUSY_POLL, &busy_poll, sizeof(busy_poll));

        struct sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);

        if (bind(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            throw std::runtime_error("Failed to bind UDP socket");
        }

        // Join multicast group
        struct ip_mreq mreq;
        inet_pton(AF_INET, ip, &mreq.imr_multiaddr.s_addr);
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if (setsockopt(sock_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
            throw std::runtime_error("Failed to join multicast group");
        }

        // Set non-blocking
        int flags = fcntl(sock_fd, F_GETFL, 0);
        fcntl(sock_fd, F_SETFL, flags | O_NONBLOCK);

        // Setup Epoll
        epoll_fd = epoll_create1(0);
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET; // Edge-triggered
        ev.data.fd = sock_fd;
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock_fd, &ev);
    }

    ~UDPListener() {
        close(sock_fd);
        close(epoll_fd);
    }

    // Call this in a tight loop pinned to an isolated core
    template <typename Callback>
    void poll(Callback on_message) {
        struct epoll_event events[16];
        // 0 timeout means non-blocking check (spin)
        int nfds = epoll_wait(epoll_fd, events, 16, 0);
        
        for (int i = 0; i < nfds; ++i) {
            if (events[i].data.fd == sock_fd) {
                drain_socket(on_message);
            }
        }
    }

    // Direct busy-spin read, bypassing epoll (for ultimate low-latency mode)
    template <typename Callback>
    void spin(Callback on_message) {
        drain_socket(on_message);
    }

private:
    template <typename Callback>
    void drain_socket(Callback on_message) {
        char buffer[65536];
        while (true) {
            ssize_t n = recv(sock_fd, buffer, sizeof(buffer), 0);
            if (n > 0) {
                on_message(buffer, n);
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break; // Empty
            } else {
                break;
            }
        }
    }
};
