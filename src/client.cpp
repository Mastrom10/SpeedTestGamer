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
#include <fstream>
#include <iomanip>
#include <ctime>

using Clock = std::chrono::high_resolution_clock;

struct Packet {
    uint32_t seq;
    uint64_t timestamp_ns; // send timestamp in nanoseconds
};

static void print_help(const char* prog) {
    std::cout << "Usage: " << prog << " -a addr -p port [-n count] [-s size]\n";
}

int main(int argc, char* argv[]) {
    std::string server_ip;
    int port = 0;
    int count = 100;
    int size = sizeof(Packet);
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") { print_help(argv[0]); return 0; }
        else if ((arg == "-a" || arg == "--addr") && i + 1 < argc) {
            server_ip = argv[++i];
        } else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if ((arg == "-n" || arg == "--count") && i + 1 < argc) {
            count = std::atoi(argv[++i]);
        } else if ((arg == "-s" || arg == "--size") && i + 1 < argc) {
            size = std::atoi(argv[++i]);
        } else {
            print_help(argv[0]);
            return 1;
        }
    }
    if (server_ip.empty() || port == 0) {
        print_help(argv[0]);
        return 1;
    }
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

    // prepare log file
    auto t = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(t);
    std::tm* tm = std::localtime(&tt);
    char fname[64];
    std::strftime(fname, sizeof(fname), "client_%Y%m%d_%H%M%S.log", tm);
    std::ofstream log(fname);

    const size_t DISPLAY = 20;
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
        log << resp.seq << "," << latency_ms << "\n";

        std::cout << "\033[2J\033[H";
        size_t start = latencies.size() > DISPLAY ? latencies.size() - DISPLAY : 0;
        for (size_t j = start; j < latencies.size(); ++j) {
            std::cout << "seq=" << j << " latency_ms=" << latencies[j] << "\n";
        }
        double sum = 0.0;
        for (double l : latencies) sum += l;
        double avg = latencies.empty() ? 0 : sum / latencies.size();
        std::cout << "Packets:" << latencies.size() << " Avg(ms):" << std::fixed << std::setprecision(2) << avg << std::flush;
    }

    double sum = 0;
    for (double l : latencies) sum += l;
    double avg = latencies.empty() ? 0 : sum / latencies.size();
    std::cout << "\nAverage latency: " << avg << " ms" << std::endl;

    delete[] buffer;
    close(sock);
    log.close();
}
