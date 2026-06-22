#include <iostream>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <chrono>
#include <thread>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <orders.bin> [ip=127.0.0.1] [port=9091]\n";
        return 1;
    }

    const char* file_path = argv[1];
    const char* ip = (argc >= 3) ? argv[2] : "127.0.0.1";
    int port = (argc >= 4) ? std::stoi(argv[3]) : 9091;

    int fd = open(file_path, O_RDONLY);
    if (fd < 0) {
        std::cerr << "Failed to open " << file_path << "\n";
        return 1;
    }

    struct stat sb;
    fstat(fd, &sb);
    size_t file_size = sb.st_size;

    // Zero-copy mmap to load the entire replay file into RAM
    char* data = static_cast<char*>(mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0));
    if (data == MAP_FAILED) {
        std::cerr << "mmap failed\n";
        close(fd);
        return 1;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Failed to connect to engine at " << ip << ":" << port << "\n";
        return 1;
    }

    // Optimize socket for maximum throughput
    int nodelay = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    
    // Increase socket send buffer
    int sndbuf = 8 * 1024 * 1024; // 8MB
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    std::cout << "Replay Framework connected to Exchange. Replaying " << file_size << " bytes.\n";

    auto start = std::chrono::high_resolution_clock::now();

    size_t offset = 0;
    const size_t CHUNK_SIZE = 65536; // 64KB chunks
    
    while (offset < file_size) {
        size_t to_send = std::min(CHUNK_SIZE, file_size - offset);
        ssize_t sent = send(sock, data + offset, to_send, 0);
        if (sent > 0) {
            offset += sent;
            // 5 microsecond sleep per 64KB chunk to pace injection rate to ~1.2M msgs/sec
            // This prevents artificial queuing delay
            std::this_thread::sleep_for(std::chrono::microseconds(5));
        } else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            __builtin_ia32_pause();
        } else {
            std::cerr << "Socket error during replay\n";
            break;
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;
    
    std::cout << "Replay Complete!\n";
    std::cout << "Time elapsed: " << diff.count() << " seconds.\n";
    std::cout << "Throughput: " << (file_size / 1024.0 / 1024.0 / diff.count()) << " MB/sec\n";

    munmap(data, file_size);
    close(fd);
    close(sock);
    
    return 0;
}
