#include <iostream>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <fstream>
#include <ctime>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <thread>
#include <atomic>

// Header sent before every payload packet. Pack the structure so the size
// matches the 20 bytes expected by the Android client.
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
    uint64_t timestamp_ns;
    uint32_t server_id;
    uint32_t tick_ms;
    std::vector<uint8_t> payload;
};

struct Sync {
    uint64_t server_time_ns;
    uint32_t server_id;
    uint32_t tick_ms;
};

constexpr int SYNC_COUNT = 5;
constexpr int SYNC_INTERVAL_MS = 1000;

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

    const uint32_t server_id = 1;
    auto t = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(t);
    std::tm* tm = std::localtime(&tt);
    char fname[64];
    std::strftime(fname, sizeof(fname), "server_%Y%m%d_%H%M%S.log", tm);
    std::ofstream log(fname);

    // Layout must match the Android client which packs these fields without
    // any padding (4*4 + 8 = 24 bytes). Use pragma pack to ensure the same
    // size across architectures.
    #pragma pack(push, 1)
    struct Request {
        uint32_t count;
        uint64_t client_time_ns;
        uint32_t client_id;
        uint32_t payload_size;
        uint32_t tick_request_ms;
    };
    #pragma pack(pop)

    while (true) {
        char buffer[1500];
        sockaddr_in client{};
        socklen_t len = sizeof(client);
        ssize_t n = recvfrom(sock, buffer, sizeof(buffer), 0, (sockaddr*)&client, &len);
        if (n < (ssize_t)sizeof(Request)) {
            perror("recvfrom");
            continue;
        }
        Request req;
        std::memcpy(&req, buffer, sizeof(req));

        log << "REQ count=" << req.count
            << " client_time_ns=" << req.client_time_ns
            << " client_id=" << req.client_id
            << " payload_size=" << req.payload_size
            << " tick_request_ms=" << req.tick_request_ms << "\n";

        uint32_t actual_tick = tick_ms > 0 ? tick_ms : req.tick_request_ms;

        // send multiple sync messages so the client can pick the one with
        // the smallest observed round-trip time for a better clock offset
        Sync sync{now_ns(), server_id, actual_tick};
        for (int i = 0; i < SYNC_COUNT; ++i) {
            sendto(sock, &sync, sizeof(sync), 0, (sockaddr*)&client, len);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        uint32_t count = req.count;
        sockaddr_in client_copy = client;
        socklen_t len_copy = len;
        std::thread([sock, client_copy, len_copy, actual_tick, count, server_id, req]() mutable {
            std::atomic<bool> running(true);
            std::thread sync_thread([&] {
                while (running.load()) {
                    Sync periodic{now_ns(), server_id, actual_tick};
                    sendto(sock, &periodic, sizeof(periodic), 0,
                           (sockaddr*)&client_copy, len_copy);
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(SYNC_INTERVAL_MS));
                }
            });

            PacketHeader hdr{};
            std::vector<uint8_t> buf(sizeof(PacketHeader) + req.payload_size, 0);
            for (uint32_t i = 0; i < count; ++i) {
                hdr.seq = i;
                hdr.timestamp_ns = now_ns();
                hdr.server_id = server_id;
                hdr.tick_ms = actual_tick;
                std::memcpy(buf.data(), &hdr, sizeof(hdr));
                sendto(sock, buf.data(), buf.size(), 0, (sockaddr*)&client_copy, len_copy);
                if (actual_tick > 0 && i + 1 < count)
                    std::this_thread::sleep_for(std::chrono::milliseconds(actual_tick));
            }

            running = false;
            sync_thread.join();
        }).detach();
    }
}
