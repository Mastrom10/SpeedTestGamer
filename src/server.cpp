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

// Simple UDP echo server that responds with received data.
int main(int argc, char* argv[]) {
    int port = 9000;
    if (argc > 1) {
        port = std::atoi(argv[1]);
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

    while (true) {
        char buffer[1500];
        sockaddr_in client{};
        socklen_t len = sizeof(client);
        ssize_t n = recvfrom(sock, buffer, sizeof(buffer), 0, (sockaddr*)&client, &len);
        if (n < 0) {
            perror("recvfrom");
            continue;
        }
        // Echo back
        sendto(sock, buffer, n, 0, (sockaddr*)&client, len);
    }
}
