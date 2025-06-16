#include <iostream>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <thread>
#include <atomic>

struct Packet {
    uint32_t seq;
    uint64_t timestamp_ns;
};

static uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
}

static void print_help(const char* prog) {
    std::cout << "Usage: " << prog << " [-p port] [-t tick_ms]\n";
}

int main(int argc, char* argv[]) {
    int port = 9000;
    int tick_ms = 0;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") { print_help(argv[0]); return 0; }
        else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if ((arg == "-t" || arg == "--tick") && i + 1 < argc) {
            tick_ms = std::atoi(argv[++i]);
        } else {
            print_help(argv[0]);
            return 1;
        }
    }

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sock);
        return 1;
    }

    std::cout << "Server listening on port " << port << std::endl;

    sockaddr_in last_client{};
    socklen_t last_len = sizeof(last_client);
    std::atomic_bool have_client(false);

    std::thread tick_thread;
    if (tick_ms > 0) {
        tick_thread = std::thread([&]() {
            Packet p{};
            while (true) {
                if (have_client.load()) {
                    p.timestamp_ns = now_ns();
                    sendto(sock, &p, sizeof(p), 0, (sockaddr*)&last_client, last_len);
                    ++p.seq;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(tick_ms));
            }
        });
    }

    while (true) {
        char buffer[1500];
        sockaddr_in client{};
        socklen_t len = sizeof(client);
        ssize_t n = recvfrom(sock, buffer, sizeof(buffer), 0, (sockaddr*)&client, &len);
        if (n < 0) {
            perror("recvfrom");
            continue;
        }
        last_client = client;
        last_len = len;
        have_client.store(true);
        sendto(sock, buffer, n, 0, (sockaddr*)&client, len);
    }

    if (tick_thread.joinable()) tick_thread.join();
}
