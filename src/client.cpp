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
#include <limits>

using Clock = std::chrono::high_resolution_clock;

struct Packet {
    uint32_t seq;
    uint64_t timestamp_ns; // send timestamp in nanoseconds
};

struct Sync {
    uint64_t server_time_ns;
};

static uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()).count();
}

struct Request {
    uint32_t count;
};

static void print_help(const char* prog) {
    std::cout << "Usage: " << prog << " -a addr -p port [-n count]\n";
}

int main(int argc, char* argv[]) {
    std::string server_ip;
    int port = 0;
    int count = 100;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") { print_help(argv[0]); return 0; }
        else if ((arg == "-a" || arg == "--addr") && i + 1 < argc) {
            server_ip = argv[++i];
        } else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if ((arg == "-n" || arg == "--count") && i + 1 < argc) {
            count = std::atoi(argv[++i]);
        } else {
            print_help(argv[0]);
            return 1;
        }
    }
    if (server_ip.empty() || port == 0) {
        print_help(argv[0]);
        return 1;
    }

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
    char* buffer = new char[sizeof(Packet)];
    double min_lat = std::numeric_limits<double>::max();
    double max_lat = 0.0;

    // prepare log file
    auto t = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(t);
    std::tm* tm = std::localtime(&tt);
    char fname[64];
    std::strftime(fname, sizeof(fname), "client_%Y%m%d_%H%M%S.log", tm);
    std::ofstream log(fname);

    const size_t DISPLAY = 5;

    // send request to server with number of packets and note send time
    Request req{static_cast<uint32_t>(count)};
    uint64_t send_ns = now_ns();
    if (sendto(sock, &req, sizeof(req), 0, (sockaddr*)&server, sizeof(server)) < 0) {
        perror("sendto");
        return 1;
    }

    // receive sync message containing server timestamp to estimate clock offset
    sockaddr_in from{};
    socklen_t from_len = sizeof(from);
    Sync sync{};
    ssize_t n = recvfrom(sock, &sync, sizeof(sync), 0, (sockaddr*)&from, &from_len);
    if (n < (ssize_t)sizeof(sync)) {
        perror("recvfrom");
        return 1;
    }
    uint64_t recv_ns = now_ns();
    int64_t offset_ns = static_cast<int64_t>(sync.server_time_ns) -
                        static_cast<int64_t>(send_ns + (recv_ns - send_ns) / 2);

    for (int i = 0; i < count; ++i) {
        sockaddr_in pkt_from{};
        socklen_t len = sizeof(pkt_from);
        ssize_t n = recvfrom(sock, buffer, sizeof(Packet), 0, (sockaddr*)&pkt_from, &len);
        if (n < (ssize_t)sizeof(Packet)) {
            perror("recvfrom");
            continue;
        }
        uint64_t now = now_ns();
        Packet resp;
        std::memcpy(&resp, buffer, sizeof(resp));
        double latency_ms = (now - (resp.timestamp_ns - offset_ns)) / 1e6;
        latencies.push_back(latency_ms);
        if (latency_ms < min_lat) min_lat = latency_ms;
        if (latency_ms > max_lat) max_lat = latency_ms;
        log << resp.seq << "," << latency_ms << "\n";

        std::cout << "\033[2J\033[H";
        size_t start = latencies.size() > DISPLAY ? latencies.size() - DISPLAY : 0;
        for (size_t j = start; j < latencies.size(); ++j) {
            std::cout << "seq=" << j << " latency_ms=" << latencies[j] << "\n";
        }
        double sum = 0.0;
        for (double l : latencies) sum += l;
        double avg = latencies.empty() ? 0 : sum / latencies.size();
        std::cout << "Packets:" << latencies.size() << " Avg(ms):" << std::fixed
                  << std::setprecision(2) << avg
                  << " Min:" << std::setprecision(2) << min_lat
                  << " Max:" << std::setprecision(2) << max_lat << std::flush;
    }

    double sum = 0;
    for (double l : latencies) sum += l;
    double avg = latencies.empty() ? 0 : sum / latencies.size();
    std::cout << "\nAvg: " << avg << " ms Min: " << min_lat
              << " ms Max: " << max_lat << " ms" << std::endl;

    delete[] buffer;
    close(sock);
    log.close();
}
