#include <iostream>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fstream>
#include <iomanip>
#include <ctime>
#include <limits>

using Clock = std::chrono::high_resolution_clock;

// Packet header layout used by the server. Pack it so the size is 20 bytes
// and consistent with the Android implementation.
#pragma pack(push, 1)
struct PacketHeader {
    uint32_t seq;
    uint64_t timestamp_ns;
    uint32_t server_id;
    uint32_t tick_ms;
};
#pragma pack(pop)

struct Packet {
    uint32_t seq;
    uint64_t timestamp_ns; // send timestamp in nanoseconds
    uint32_t server_id;
    uint32_t tick_ms;
    std::vector<uint8_t> payload;
};

struct SyncRequest {
    uint64_t client_time_ns; // t0
};

struct SyncResponse {
    uint64_t recv_time_ns; // t1
    uint64_t send_time_ns; // t2
};

constexpr int INIT_SYNC_COUNT = 10;
constexpr uint64_t SYNC_INTERVAL_NS = 5ULL * 1000ULL * 1000ULL * 1000ULL;

static uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()).count();
}

// Same layout as used by the Android client (packed to 24 bytes).
#pragma pack(push, 1)
struct Request {
    uint32_t count;
    uint64_t client_time_ns;
    uint32_t client_id;
    uint32_t payload_size;
    uint32_t tick_request_ms;
};
#pragma pack(pop)

static void print_help(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "  -a, --addr <ip>     Server IPv4 address\n"
              << "  -p, --port <port>   Server UDP port\n"
              << "  -n, --count <num>   Number of packets to request (default 100)\n"
              << "  -t, --tick <ms>     Desired tick interval in ms\n"
              << "  -s, --payload <b>   Payload size in bytes\n"
              << "  -i, --id <id>       Optional client identifier\n"
              << "  -h, --help          Show this help message\n";
}

int main(int argc, char* argv[]) {
    std::string server_ip;
    int port = 0;
    int count = 100;
    uint32_t client_id = 0;
    uint32_t payload_size = 0;
    uint32_t tick_request_ms = 0;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") { print_help(argv[0]); return 0; }
        else if ((arg == "-a" || arg == "--addr") && i + 1 < argc) {
            server_ip = argv[++i];
        } else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if ((arg == "-n" || arg == "--count") && i + 1 < argc) {
            count = std::atoi(argv[++i]);
        } else if ((arg == "-t" || arg == "--tick") && i + 1 < argc) {
            tick_request_ms = std::atoi(argv[++i]);
        } else if ((arg == "-s" || arg == "--payload") && i + 1 < argc) {
            payload_size = std::atoi(argv[++i]);
        } else if ((arg == "-i" || arg == "--id") && i + 1 < argc) {
            client_id = std::atoi(argv[++i]);
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
    char* buffer = new char[sizeof(PacketHeader) + payload_size];
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

    // initial clock synchronization
    sockaddr_in from{};
    socklen_t from_len = sizeof(from);
    uint64_t best_rtt = std::numeric_limits<uint64_t>::max();
    int64_t offset_ns = 0;
    for (int i = 0; i < INIT_SYNC_COUNT; ++i) {
        SyncRequest sreq{now_ns()};
        uint64_t t0 = sreq.client_time_ns;
        if (sendto(sock, &sreq, sizeof(sreq), 0, (sockaddr*)&server, sizeof(server)) < 0) {
            perror("sendto");
            return 1;
        }
        SyncResponse sresp{};
        ssize_t n = recvfrom(sock, &sresp, sizeof(sresp), 0,
                             (sockaddr*)&from, &from_len);
        if (n < (ssize_t)sizeof(sresp)) {
            perror("recvfrom");
            return 1;
        }
        uint64_t t3 = now_ns();
        uint64_t rtt = (t3 - t0) - (sresp.send_time_ns - sresp.recv_time_ns);
        int64_t off = ((int64_t)sresp.recv_time_ns - (int64_t)t0 +
                       (int64_t)sresp.send_time_ns - (int64_t)t3) / 2;
        if (rtt < best_rtt) {
            best_rtt = rtt;
            offset_ns = off;
        }
    }

    log << "Initial sync offset_ns=" << offset_ns
        << " rtt_ns=" << best_rtt << "\n";

    // send request to server
    Request req{};
    req.count = static_cast<uint32_t>(count);
    req.client_time_ns = now_ns();
    req.client_id = client_id;
    req.payload_size = payload_size;
    req.tick_request_ms = tick_request_ms;
    if (sendto(sock, &req, sizeof(req), 0, (sockaddr*)&server, sizeof(server)) < 0) {
        perror("sendto");
        return 1;
    }

    log << "SEND Request count=" << req.count << " client_time_ns=" << req.client_time_ns
        << " client_id=" << req.client_id << " payload_size=" << req.payload_size
        << " tick_request_ms=" << req.tick_request_ms << "\n";

    uint64_t next_sync = now_ns() + SYNC_INTERVAL_NS;
    bool waiting_sync = false;
    uint64_t sync_t0 = 0;

    for (int i = 0; i < count; ) {
        uint64_t now = now_ns();
        if (!waiting_sync && now >= next_sync) {
            SyncRequest sreq{now};
            sync_t0 = sreq.client_time_ns;
            sendto(sock, &sreq, sizeof(sreq), 0, (sockaddr*)&server, sizeof(server));
            waiting_sync = true;
            next_sync = now + SYNC_INTERVAL_NS;
        }

        sockaddr_in pkt_from{};
        socklen_t len = sizeof(pkt_from);
        ssize_t n = recvfrom(sock, buffer, sizeof(PacketHeader) + payload_size, 0,
                             (sockaddr*)&pkt_from, &len);
        if (n < 0) {
            perror("recvfrom");
            continue;
        }

        uint64_t recv_time = now_ns();
        if (waiting_sync && n == (ssize_t)sizeof(SyncResponse)) {
            SyncResponse resp{};
            std::memcpy(&resp, buffer, sizeof(resp));
            uint64_t rtt = (recv_time - sync_t0) - (resp.send_time_ns - resp.recv_time_ns);
            int64_t off = ((int64_t)resp.recv_time_ns - (int64_t)sync_t0 +
                           (int64_t)resp.send_time_ns - (int64_t)recv_time) / 2;
            if (rtt < best_rtt) {
                best_rtt = rtt;
                offset_ns = off;
                log << "SYNC update offset_ns=" << offset_ns
                    << " rtt_ns=" << rtt << "\n";
            }
            waiting_sync = false;
            continue;
        }

        if (n < (ssize_t)sizeof(PacketHeader)) {
            perror("recvfrom");
            continue;
        }
        PacketHeader hdr{};
        std::memcpy(&hdr, buffer, sizeof(hdr));
        Packet resp;
        resp.seq = hdr.seq;
        resp.timestamp_ns = hdr.timestamp_ns;
        resp.server_id = hdr.server_id;
        resp.tick_ms = hdr.tick_ms;
        resp.payload.resize(payload_size);
        if (payload_size && (size_t)n > sizeof(PacketHeader))
            std::memcpy(resp.payload.data(), buffer + sizeof(PacketHeader),
                        std::min<size_t>(payload_size, n - sizeof(PacketHeader)));

        double latency_ms = (recv_time - (resp.timestamp_ns - offset_ns)) / 1e6;
        latencies.push_back(latency_ms);
        if (latency_ms < min_lat) min_lat = latency_ms;
        if (latency_ms > max_lat) max_lat = latency_ms;
        log << "RECV Packet seq=" << resp.seq << " server_id=" << resp.server_id
            << " tick_ms=" << resp.tick_ms << " latency_ms=" << latency_ms << "\n";

        std::cout << "\033[2J\033[H";
        size_t start = latencies.size() > DISPLAY ? latencies.size() - DISPLAY : 0;
        for (size_t j = start; j < latencies.size(); ++j) {
            std::cout << "seq=" << j << " latency_ms=" << latencies[j] << "\n";
        }
        std::cout << "server_id=" << resp.server_id
                  << " tick_ms=" << resp.tick_ms
                  << " payload=" << resp.payload.size() << " bytes\n";
        double sum = 0.0;
        for (double l : latencies) sum += l;
        double avg = latencies.empty() ? 0 : sum / latencies.size();
        std::cout << "Packets:" << latencies.size() << " Avg(ms):" << std::fixed
                  << std::setprecision(2) << avg
                  << " Min:" << std::setprecision(2) << min_lat
                  << " Max:" << std::setprecision(2) << max_lat << std::flush;

        ++i;
    }

    double sum = 0;
    for (double l : latencies) sum += l;
    double avg = latencies.empty() ? 0 : sum / latencies.size();
    std::cout << "\nAvg: " << avg << " ms Min: " << min_lat
              << " ms Max: " << max_lat
              << " ms Offset: " << offset_ns / 1e6 << " ms" << std::endl;

    delete[] buffer;
    close(sock);
    log.close();
}
