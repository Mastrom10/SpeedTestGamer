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

using Clock = std::chrono::high_resolution_clock;

struct Packet {
    uint32_t seq;
    uint64_t timestamp_ns; // send timestamp in nanoseconds
};

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <server_ip> <server_port> [count] [size]" << std::endl;
        return 1;
    }
    std::string server_ip = argv[1];
    int port = std::atoi(argv[2]);
    int count = argc > 3 ? std::atoi(argv[3]) : 100;
    int size = argc > 4 ? std::atoi(argv[4]) : sizeof(Packet);
    if (size < (int)sizeof(Packet)) size = sizeof(Packet);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    if (inet_pton(AF_INET, server_ip.c_str(), &server.sin_addr) != 1) {
        std::cerr << "Invalid server IP" << std::endl;
        return 1;
    }

    std::vector<double> latencies;
    char* buffer = new char[size];

    for (int i = 0; i < count; ++i) {
        Packet p{static_cast<uint32_t>(i),
                 (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count()};
        std::memcpy(buffer, &p, sizeof(p));
        if (sendto(sock, buffer, size, 0, (sockaddr*)&server, sizeof(server)) < 0) {
            perror("sendto");
            continue;
        }
        sockaddr_in from{};
        socklen_t len = sizeof(from);
        ssize_t n = recvfrom(sock, buffer, size, 0, (sockaddr*)&from, &len);
        if (n < (ssize_t)sizeof(Packet)) {
            perror("recvfrom");
            continue;
        }
        uint64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
        Packet resp;
        std::memcpy(&resp, buffer, sizeof(resp));
        double latency_ms = (now_ns - resp.timestamp_ns) / 1e6;
        latencies.push_back(latency_ms);
        std::cout << "seq=" << resp.seq << " latency_ms=" << latency_ms << std::endl;
    }

    double sum = 0;
    for (double l : latencies) sum += l;
    double avg = latencies.empty() ? 0 : sum / latencies.size();
    std::cout << "Average latency: " << avg << " ms" << std::endl;

    delete[] buffer;
    close(sock);
}
