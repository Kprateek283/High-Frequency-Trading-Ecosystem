#pragma once
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <stdexcept>
#include <functional>
#include <iostream>

class TCPClient {
private:
    int sock_fd;
    int epoll_fd;
    bool connected = false;

public:
    TCPClient() {
        sock_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (sock_fd < 0) {
            throw std::runtime_error("Failed to create TCP socket");
        }

        // Disable Nagle's algorithm for minimum latency
        int nodelay = 1;
        setsockopt(sock_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        // Enable Busy Polling
        int busy_poll = 50;
        setsockopt(sock_fd, SOL_SOCKET, SO_BUSY_POLL, &busy_poll, sizeof(busy_poll));

        // Setup Epoll
        epoll_fd = epoll_create1(0);
    }

    ~TCPClient() {
        if (sock_fd >= 0) close(sock_fd);
        if (epoll_fd >= 0) close(epoll_fd);
    }

    bool connect_to(const char* ip, int port, bool blocking = false) {
        struct sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, ip, &addr.sin_addr);

        if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "connect() failed, errno: " << errno << " (" << strerror(errno) << ")\n";
            return false;
        }

        if (!blocking) {
            int flags = fcntl(sock_fd, F_GETFL, 0);
            fcntl(sock_fd, F_SETFL, flags | O_NONBLOCK);
        }

        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = sock_fd;
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock_fd, &ev);

        connected = true;
        return true;
    }

    bool is_connected() const {
        return connected;
    }

    inline bool send_bytes(const void* data, size_t len) {
        size_t total_sent = 0;
        const char* buf = static_cast<const char*>(data);
        while (total_sent < len) {
            ssize_t sent = send(sock_fd, buf + total_sent, len - total_sent, MSG_NOSIGNAL);
            if (sent > 0) {
                total_sent += sent;
            } else if (sent < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (total_sent == 0) {
                        return false; 
                    } else {
                        __builtin_ia32_pause();
                        continue; // Spin internally for the remainder to avoid corruption
                    }
                } else if (errno == EINTR) {
                    continue;
                } else {
                    std::cerr << "send_bytes fatal error: " << errno << " (" << strerror(errno) << ")\n";
                    connected = false;
                    return false; // Fatal error
                }
            } else {
                std::cerr << "send_bytes connection closed by peer\n";
                connected = false;
                return false; // Connection closed
            }
        }
        return true;
    }

    // Event-driven poll (CPU friendly)
    template <typename Callback>
    void poll(Callback on_message) {
        if (!connected) return;

        struct epoll_event events[16];
        int nfds = epoll_wait(epoll_fd, events, 16, 0);
        
        for (int i = 0; i < nfds; ++i) {
            if (events[i].data.fd == sock_fd) {
                drain_socket(on_message);
            }
        }
    }

    // Direct busy-spin read (Lowest latency, 100% CPU usage)
    template <typename Callback>
    void spin(Callback on_message) {
        if (!connected) return;
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
                connected = false;
                break;
            }
        }
    }
};
