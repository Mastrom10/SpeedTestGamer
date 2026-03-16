#include <arpa/inet.h>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <net/if.h>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unordered_map>
#include <unistd.h>
#include <vector>
#include <atomic>
#include <linux/wireless.h>

namespace {

using SteadyClock = std::chrono::steady_clock;
using SystemClock = std::chrono::system_clock;

constexpr uint16_t UDP_PROTOCOL_VERSION = 2;
constexpr uint16_t TCP_PROTOCOL_VERSION = 1;
constexpr uint32_t UDP_MAX_PAYLOAD_BYTES = 1024;
constexpr uint32_t UDP_MAX_PACKET_COUNT = 12000;
constexpr uint32_t UDP_MIN_TICK_MS = 1;
constexpr uint32_t UDP_MAX_TICK_MS = 2000;
constexpr size_t UDP_MAX_DATAGRAM_BYTES = 1400;
constexpr uint32_t TCP_MAGIC = 0x53544754; // "TGTS"
constexpr uint32_t TCP_DEFAULT_CHUNK_BYTES = 16 * 1024;
constexpr uint32_t TCP_MIN_CHUNK_BYTES = 256;
constexpr uint32_t TCP_MAX_CHUNK_BYTES = 64 * 1024;
constexpr uint32_t TCP_MIN_DURATION_MS = 1000;
constexpr uint32_t TCP_MAX_DURATION_MS = 60000;
constexpr int SESSION_IDLE_TIMEOUT_MS = 30000;

enum class ServerLinkType : uint8_t {
    UNKNOWN = 0,
    ETHERNET = 1,
    WIFI = 2,
    CELLULAR = 3,
    OTHER = 4,
};

enum class UdpMessageType : uint16_t {
    SYNC_REQ = 1,
    SYNC_RESP = 2,
    TEST_START_REQ = 3,
    TEST_START_ACK = 4,
    UP_TICK = 5,
    DOWN_TICK = 6,
    TEST_END_REQ = 7,
    TEST_END_SUMMARY = 8,
};

enum class TcpMessageType : uint16_t {
    START_REQ = 1,
    START_ACK = 2,
    DATA = 3,
    STOP = 4,
    RESULT = 5,
    BUSY = 6,
};

enum class ThroughputDirection : uint8_t {
    DOWNLOAD = 1,
    UPLOAD = 2,
};

enum class LogLevel : int {
    SUMMARY = 0,
    EVENTS = 1,
    VERBOSE = 2,
};

struct UdpHeader {
    UdpMessageType type;
    uint16_t version;
    uint32_t sessionId;
    uint32_t seq;
};

struct TcpHeader {
    uint32_t magic;
    uint16_t version;
    TcpMessageType type;
    uint32_t sessionId;
    uint32_t length;
};

struct UdpSessionKey {
    uint32_t sessionId;
    uint32_t ip;
    uint16_t port;

    bool operator==(const UdpSessionKey& other) const {
        return sessionId == other.sessionId && ip == other.ip && port == other.port;
    }
};

struct UdpSessionKeyHash {
    size_t operator()(const UdpSessionKey& key) const {
        size_t h1 = std::hash<uint32_t>{}(key.sessionId);
        size_t h2 = std::hash<uint32_t>{}(key.ip);
        size_t h3 = std::hash<uint16_t>{}(key.port);
        return h1 ^ (h2 << 1U) ^ (h3 << 2U);
    }
};

struct UdpSession {
    uint32_t sessionId = 0;
    sockaddr_in client{};
    uint32_t tickMs = 15;
    uint32_t expectedCount = 0;
    uint32_t payloadUpBytes = 64;
    uint32_t payloadDownBytes = 64;
    uint32_t upReceivedCount = 0;
    uint32_t downSentCount = 0;
    uint32_t upOutOfOrderCount = 0;
    int64_t maxSeqSeen = -1;
    std::vector<uint8_t> upBitmap;
    uint64_t startedNs = 0;
    uint64_t lastActivityNs = 0;
};

struct ServerOptions {
    int port = 9000;
    int tickOverrideMs = 0;
    int maxSessions = 50;
    std::string logDir = ".";
    LogLevel logLevel = LogLevel::SUMMARY;
};

struct TrafficCounters {
    std::atomic<uint64_t> udpPacketsIn{0};
    std::atomic<uint64_t> udpPacketsOut{0};
    std::atomic<uint64_t> tcpBytesIn{0};
    std::atomic<uint64_t> tcpBytesOut{0};
};

struct ServerLinkSnapshot {
    std::string iface;
    ServerLinkType type = ServerLinkType::UNKNOWN;
    uint32_t downMbps = 0;
    uint32_t upMbps = 0;
};

uint64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               SteadyClock::now().time_since_epoch())
        .count();
}

uint64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               SystemClock::now().time_since_epoch())
        .count();
}

std::string nowDateKey() {
    auto now = SystemClock::now();
    std::time_t t = SystemClock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y%m%d");
    return oss.str();
}

std::string jsonEscape(const std::string& input) {
    std::ostringstream out;
    for (char c : input) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(static_cast<unsigned char>(c)) << std::dec;
                } else {
                    out << c;
                }
                break;
        }
    }
    return out.str();
}

std::string addrToString(const sockaddr_in& addr) {
    char ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    std::ostringstream oss;
    oss << ip << ":" << ntohs(addr.sin_port);
    return oss.str();
}

std::string trim(const std::string& input) {
    size_t first = 0;
    while (first < input.size() && std::isspace(static_cast<unsigned char>(input[first]))) {
        ++first;
    }
    size_t last = input.size();
    while (last > first && std::isspace(static_cast<unsigned char>(input[last - 1]))) {
        --last;
    }
    return input.substr(first, last - first);
}

std::string readFirstLine(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) {
        return "";
    }
    std::string line;
    std::getline(in, line);
    return trim(line);
}

int readIntFile(const std::string& path, int fallback = -1) {
    std::ifstream in(path);
    if (!in.is_open()) {
        return fallback;
    }
    int value = fallback;
    in >> value;
    return in.fail() ? fallback : value;
}

bool pathExists(const std::string& path) {
    return access(path.c_str(), F_OK) == 0;
}

std::vector<std::string> listInterfaces() {
    std::vector<std::string> ifaces;
    DIR* dir = opendir("/sys/class/net");
    if (!dir) {
        return ifaces;
    }

    while (true) {
        dirent* entry = readdir(dir);
        if (!entry) {
            break;
        }
        std::string name = entry->d_name;
        if (name == "." || name == ".." || name == "lo") {
            continue;
        }
        ifaces.push_back(name);
    }
    closedir(dir);
    return ifaces;
}

bool interfaceIsUp(const std::string& iface) {
    const std::string state = readFirstLine("/sys/class/net/" + iface + "/operstate");
    return state == "up" || state == "unknown";
}

int detectWifiLinkMbps(const std::string& iface) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        return -1;
    }

    iwreq req {};
    std::memset(&req, 0, sizeof(req));
    std::snprintf(req.ifr_name, IFNAMSIZ, "%s", iface.c_str());

    int mbps = -1;
    if (ioctl(sock, SIOCGIWRATE, &req) == 0) {
        const int64_t bitsPerSec = static_cast<int64_t>(req.u.bitrate.value);
        if (bitsPerSec > 0) {
            mbps = static_cast<int>((bitsPerSec + 500000LL) / 1000000LL);
        }
    }

    close(sock);
    return mbps;
}

ServerLinkType detectInterfaceType(const std::string& iface) {
    if (pathExists("/sys/class/net/" + iface + "/wireless")) {
        return ServerLinkType::WIFI;
    }
    const int rawType = readIntFile("/sys/class/net/" + iface + "/type", -1);
    if (rawType == 1) {
        return ServerLinkType::ETHERNET;
    }
    return ServerLinkType::OTHER;
}

std::string serverLinkTypeToString(ServerLinkType type) {
    switch (type) {
        case ServerLinkType::ETHERNET: return "ethernet";
        case ServerLinkType::WIFI: return "wifi";
        case ServerLinkType::CELLULAR: return "cellular";
        case ServerLinkType::OTHER: return "other";
        case ServerLinkType::UNKNOWN:
        default:
            return "unknown";
    }
}

std::string getDefaultRouteInterface() {
    std::ifstream in("/proc/net/route");
    if (!in.is_open()) {
        return "";
    }

    std::string line;
    std::getline(in, line); // header
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        std::istringstream iss(line);
        std::string iface;
        std::string destination;
        std::string gateway;
        std::string flags;
        if (!(iss >> iface >> destination >> gateway >> flags)) {
            continue;
        }
        if (destination == "00000000") {
            return iface;
        }
    }
    return "";
}

ServerLinkSnapshot detectServerLinkSnapshot() {
    ServerLinkSnapshot snapshot;
    std::vector<std::string> candidates;

    const std::string defaultIface = getDefaultRouteInterface();
    if (!defaultIface.empty()) {
        candidates.push_back(defaultIface);
    }

    for (const auto& iface : listInterfaces()) {
        if (iface == defaultIface) {
            continue;
        }
        candidates.push_back(iface);
    }

    for (const auto& iface : candidates) {
        if (!interfaceIsUp(iface)) {
            continue;
        }

        snapshot.iface = iface;
        snapshot.type = detectInterfaceType(iface);

        int speedMbps = readIntFile("/sys/class/net/" + iface + "/speed", -1);
        if (speedMbps <= 0 && snapshot.type == ServerLinkType::WIFI) {
            speedMbps = detectWifiLinkMbps(iface);
        }
        if (speedMbps > 0) {
            snapshot.downMbps = static_cast<uint32_t>(speedMbps);
            snapshot.upMbps = static_cast<uint32_t>(speedMbps);
        } else {
            snapshot.downMbps = 0;
            snapshot.upMbps = 0;
        }
        return snapshot;
    }

    if (!defaultIface.empty()) {
        snapshot.iface = defaultIface;
        snapshot.type = detectInterfaceType(defaultIface);
    }
    return snapshot;
}

class JsonLogger {
public:
    JsonLogger(std::string directory, LogLevel level)
        : dir_(std::move(directory)), level_(level) {
        rotateIfNeeded();
    }

    void log(LogLevel level, const std::string& event, const std::string& extraJson = "") {
        if (static_cast<int>(level) > static_cast<int>(level_)) {
            return;
        }
        std::lock_guard<std::mutex> lock(mu_);
        rotateIfNeeded();

        file_ << "{\"tsMs\":" << nowMs()
              << ",\"event\":\"" << jsonEscape(event) << "\"";
        if (!extraJson.empty()) {
            file_ << "," << extraJson;
        }
        file_ << "}\n";
        file_.flush();
    }

private:
    void rotateIfNeeded() {
        const std::string key = nowDateKey();
        if (file_.is_open() && key == currentKey_) {
            return;
        }
        if (file_.is_open()) {
            file_.close();
        }

        currentKey_ = key;
        std::string path = dir_ + "/server_" + key + ".jsonl";
        file_.open(path, std::ios::out | std::ios::app);
        if (!file_.is_open()) {
            std::cerr << "No se pudo abrir log en " << path << std::endl;
        }
    }

    std::mutex mu_;
    std::string dir_;
    LogLevel level_;
    std::string currentKey_;
    std::ofstream file_;
};

template <typename T>
void appendLe(std::vector<uint8_t>& buffer, T value) {
    for (size_t i = 0; i < sizeof(T); ++i) {
        buffer.push_back(static_cast<uint8_t>((static_cast<uint64_t>(value) >> (8U * i)) & 0xFFU));
    }
}

template <typename T>
bool readLe(const uint8_t* data, size_t size, size_t& offset, T& out) {
    if (offset + sizeof(T) > size) {
        return false;
    }
    uint64_t value = 0;
    for (size_t i = 0; i < sizeof(T); ++i) {
        value |= static_cast<uint64_t>(data[offset + i]) << (8U * i);
    }
    out = static_cast<T>(value);
    offset += sizeof(T);
    return true;
}

std::vector<uint8_t> makeUdpPacket(UdpMessageType type, uint32_t sessionId, uint32_t seq, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> packet;
    packet.reserve(12 + body.size());
    appendLe<uint16_t>(packet, static_cast<uint16_t>(type));
    appendLe<uint16_t>(packet, UDP_PROTOCOL_VERSION);
    appendLe<uint32_t>(packet, sessionId);
    appendLe<uint32_t>(packet, seq);
    packet.insert(packet.end(), body.begin(), body.end());
    return packet;
}

bool parseUdpHeader(const uint8_t* data, size_t size, UdpHeader& header) {
    if (size < 12) {
        return false;
    }
    size_t offset = 0;
    uint16_t type = 0;
    if (!readLe<uint16_t>(data, size, offset, type)) return false;
    if (!readLe<uint16_t>(data, size, offset, header.version)) return false;
    if (!readLe<uint32_t>(data, size, offset, header.sessionId)) return false;
    if (!readLe<uint32_t>(data, size, offset, header.seq)) return false;

    header.type = static_cast<UdpMessageType>(type);
    return true;
}

std::vector<uint8_t> makeTcpFrame(TcpMessageType type, uint32_t sessionId, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> frame;
    frame.reserve(16 + body.size());
    appendLe<uint32_t>(frame, TCP_MAGIC);
    appendLe<uint16_t>(frame, TCP_PROTOCOL_VERSION);
    appendLe<uint16_t>(frame, static_cast<uint16_t>(type));
    appendLe<uint32_t>(frame, sessionId);
    appendLe<uint32_t>(frame, static_cast<uint32_t>(body.size()));
    frame.insert(frame.end(), body.begin(), body.end());
    return frame;
}

bool readExact(int fd, uint8_t* out, size_t bytes) {
    size_t readTotal = 0;
    while (readTotal < bytes) {
        ssize_t n = recv(fd, out + readTotal, bytes - readTotal, 0);
        if (n == 0) {
            return false;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return false;
            }
            return false;
        }
        readTotal += static_cast<size_t>(n);
    }
    return true;
}

bool writeAll(int fd, const uint8_t* data, size_t bytes) {
    size_t sent = 0;
    while (sent < bytes) {
        ssize_t n = send(fd, data + sent, bytes - sent, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool readTcpFrame(int fd, TcpHeader& header, std::vector<uint8_t>& body) {
    uint8_t headerBuf[16];
    if (!readExact(fd, headerBuf, sizeof(headerBuf))) {
        return false;
    }

    size_t offset = 0;
    uint16_t typeRaw = 0;
    if (!readLe<uint32_t>(headerBuf, sizeof(headerBuf), offset, header.magic)) return false;
    if (!readLe<uint16_t>(headerBuf, sizeof(headerBuf), offset, header.version)) return false;
    if (!readLe<uint16_t>(headerBuf, sizeof(headerBuf), offset, typeRaw)) return false;
    if (!readLe<uint32_t>(headerBuf, sizeof(headerBuf), offset, header.sessionId)) return false;
    if (!readLe<uint32_t>(headerBuf, sizeof(headerBuf), offset, header.length)) return false;

    header.type = static_cast<TcpMessageType>(typeRaw);

    if (header.magic != TCP_MAGIC || header.version != TCP_PROTOCOL_VERSION) {
        return false;
    }

    body.assign(header.length, 0);
    if (header.length > 0 && !readExact(fd, body.data(), header.length)) {
        return false;
    }
    return true;
}

void printHelp(const char* prog) {
    std::cout
        << "Usage: " << prog << " [options]\n"
        << "  -p, --port <port>           Puerto UDP/TCP (default 9000)\n"
        << "  -t, --tick <ms>             Override global tick UDP (>0)\n"
        << "      --max-sessions <n>      Sesiones simultáneas máximas (default 50)\n"
        << "      --log-dir <path>        Directorio de logs JSONL (default .)\n"
        << "      --log-level <level>     summary|events|verbose (default summary)\n"
        << "  -h, --help                  Mostrar ayuda\n";
}

LogLevel parseLogLevel(const std::string& value) {
    if (value == "summary") return LogLevel::SUMMARY;
    if (value == "events") return LogLevel::EVENTS;
    if (value == "verbose") return LogLevel::VERBOSE;
    return LogLevel::SUMMARY;
}

bool parseOptions(int argc, char* argv[], ServerOptions& options) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            printHelp(argv[0]);
            return false;
        }
        if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            options.port = std::atoi(argv[++i]);
            continue;
        }
        if ((arg == "-t" || arg == "--tick") && i + 1 < argc) {
            options.tickOverrideMs = std::atoi(argv[++i]);
            continue;
        }
        if (arg == "--max-sessions" && i + 1 < argc) {
            options.maxSessions = std::atoi(argv[++i]);
            continue;
        }
        if (arg == "--log-dir" && i + 1 < argc) {
            options.logDir = argv[++i];
            continue;
        }
        if (arg == "--log-level" && i + 1 < argc) {
            options.logLevel = parseLogLevel(argv[++i]);
            continue;
        }

        std::cerr << "Opción inválida: " << arg << std::endl;
        printHelp(argv[0]);
        return false;
    }

    if (options.port <= 0 || options.port > 65535) {
        std::cerr << "Puerto inválido" << std::endl;
        return false;
    }
    if (options.maxSessions <= 0) {
        std::cerr << "--max-sessions debe ser > 0" << std::endl;
        return false;
    }
    return true;
}

int createUdpSocket(int port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket UDP");
        return -1;
    }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind UDP");
        close(fd);
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

int createTcpSocket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket TCP");
        return -1;
    }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind TCP");
        close(fd);
        return -1;
    }

    if (listen(fd, 128) < 0) {
        perror("listen TCP");
        close(fd);
        return -1;
    }

    return fd;
}

std::string safeSessionTag(uint32_t sessionId, const sockaddr_in& client) {
    std::ostringstream oss;
    oss << sessionId << "@" << addrToString(client);
    return oss.str();
}

} // namespace

int main(int argc, char* argv[]) {
    ServerOptions options;
    if (!parseOptions(argc, argv, options)) {
        return 0;
    }

    int udpFd = createUdpSocket(options.port);
    if (udpFd < 0) {
        return 1;
    }
    int tcpFd = createTcpSocket(options.port);
    if (tcpFd < 0) {
        close(udpFd);
        return 1;
    }

    std::atomic<bool> running{true};
    std::atomic<int> activeSessions{0};
    TrafficCounters counters;
    JsonLogger logger(options.logDir, options.logLevel);
    const ServerLinkSnapshot serverLink = detectServerLinkSnapshot();

    std::mutex udpMutex;
    std::unordered_map<UdpSessionKey, UdpSession, UdpSessionKeyHash> udpSessions;

    logger.log(LogLevel::SUMMARY,
               "server_start",
               "\"port\":" + std::to_string(options.port) +
                   ",\"tickOverrideMs\":" + std::to_string(options.tickOverrideMs) +
                   ",\"maxSessions\":" + std::to_string(options.maxSessions) +
                   ",\"serverIface\":\"" + jsonEscape(serverLink.iface) + "\"" +
                   ",\"serverLinkType\":\"" + jsonEscape(serverLinkTypeToString(serverLink.type)) + "\"" +
                   ",\"serverLinkDownMbps\":" + std::to_string(serverLink.downMbps) +
                   ",\"serverLinkUpMbps\":" + std::to_string(serverLink.upMbps));

    auto removeUdpSession = [&](const UdpSessionKey& key, const char* reason) {
        std::lock_guard<std::mutex> lock(udpMutex);
        auto it = udpSessions.find(key);
        if (it == udpSessions.end()) {
            return;
        }

        logger.log(LogLevel::SUMMARY,
                   "session_end",
                   "\"transport\":\"udp\",\"session\":\"" + jsonEscape(safeSessionTag(it->second.sessionId, it->second.client)) +
                       "\",\"reason\":\"" + jsonEscape(reason) + "\",\"expectedCount\":" + std::to_string(it->second.expectedCount) +
                       ",\"upReceived\":" + std::to_string(it->second.upReceivedCount) +
                       ",\"downSent\":" + std::to_string(it->second.downSentCount) +
                       ",\"upOutOfOrder\":" + std::to_string(it->second.upOutOfOrderCount));

        udpSessions.erase(it);
        activeSessions.fetch_sub(1);
    };

    std::thread statsThread([&]() {
        uint64_t prevUdpIn = 0;
        uint64_t prevUdpOut = 0;
        uint64_t prevTcpIn = 0;
        uint64_t prevTcpOut = 0;
        while (running.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            if (!running.load()) {
                break;
            }
            uint64_t curUdpIn = counters.udpPacketsIn.load();
            uint64_t curUdpOut = counters.udpPacketsOut.load();
            uint64_t curTcpIn = counters.tcpBytesIn.load();
            uint64_t curTcpOut = counters.tcpBytesOut.load();

            logger.log(LogLevel::SUMMARY,
                       "server_stats",
                       "\"activeSessions\":" + std::to_string(activeSessions.load()) +
                           ",\"udpPacketsIn\":" + std::to_string(curUdpIn) +
                           ",\"udpPacketsOut\":" + std::to_string(curUdpOut) +
                           ",\"tcpBytesIn\":" + std::to_string(curTcpIn) +
                           ",\"tcpBytesOut\":" + std::to_string(curTcpOut) +
                           ",\"udpPacketsInDelta\":" + std::to_string(curUdpIn - prevUdpIn) +
                           ",\"udpPacketsOutDelta\":" + std::to_string(curUdpOut - prevUdpOut) +
                           ",\"tcpBytesInDelta\":" + std::to_string(curTcpIn - prevTcpIn) +
                           ",\"tcpBytesOutDelta\":" + std::to_string(curTcpOut - prevTcpOut));

            prevUdpIn = curUdpIn;
            prevUdpOut = curUdpOut;
            prevTcpIn = curTcpIn;
            prevTcpOut = curTcpOut;
        }
    });

    std::thread tcpAcceptThread([&]() {
        while (running.load()) {
            sockaddr_in client{};
            socklen_t len = sizeof(client);
            int clientFd = accept(tcpFd, reinterpret_cast<sockaddr*>(&client), &len);
            if (clientFd < 0) {
                if (errno == EINTR) {
                    continue;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            if (activeSessions.load() >= options.maxSessions) {
                std::vector<uint8_t> busyBody;
                appendLe<uint32_t>(busyBody, 1000U);
                auto frame = makeTcpFrame(TcpMessageType::BUSY, 0, busyBody);
                writeAll(clientFd, frame.data(), frame.size());
                close(clientFd);
                logger.log(LogLevel::EVENTS,
                           "session_rejected",
                           "\"transport\":\"tcp\",\"client\":\"" + jsonEscape(addrToString(client)) +
                               "\",\"reason\":\"busy\"");
                continue;
            }

            activeSessions.fetch_add(1);
            std::thread([&, clientFd, client]() {
                auto finish = [&]() {
                    close(clientFd);
                    activeSessions.fetch_sub(1);
                };

                timeval tv{};
                tv.tv_sec = 1;
                tv.tv_usec = 0;
                setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                setsockopt(clientFd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

                TcpHeader startHeader{};
                std::vector<uint8_t> startBody;
                if (!readTcpFrame(clientFd, startHeader, startBody) || startHeader.type != TcpMessageType::START_REQ) {
                    logger.log(LogLevel::EVENTS,
                               "session_error",
                               "\"transport\":\"tcp\",\"client\":\"" + jsonEscape(addrToString(client)) +
                                   "\",\"reason\":\"invalid_start\"");
                    finish();
                    return;
                }

                size_t offset = 0;
                uint8_t directionRaw = 0;
                uint8_t reserved = 0;
                uint16_t reserved16 = 0;
                uint32_t durationMs = 0;
                uint32_t chunkBytes = 0;
                if (!readLe<uint8_t>(startBody.data(), startBody.size(), offset, directionRaw) ||
                    !readLe<uint8_t>(startBody.data(), startBody.size(), offset, reserved) ||
                    !readLe<uint16_t>(startBody.data(), startBody.size(), offset, reserved16) ||
                    !readLe<uint32_t>(startBody.data(), startBody.size(), offset, durationMs) ||
                    !readLe<uint32_t>(startBody.data(), startBody.size(), offset, chunkBytes) ||
                    offset != startBody.size()) {
                    logger.log(LogLevel::EVENTS,
                               "session_error",
                               "\"transport\":\"tcp\",\"sessionId\":" + std::to_string(startHeader.sessionId) +
                                   ",\"reason\":\"bad_start_payload\"");
                    finish();
                    return;
                }

                if (durationMs < TCP_MIN_DURATION_MS || durationMs > TCP_MAX_DURATION_MS) {
                    durationMs = 12000;
                }
                if (chunkBytes < TCP_MIN_CHUNK_BYTES || chunkBytes > TCP_MAX_CHUNK_BYTES) {
                    chunkBytes = TCP_DEFAULT_CHUNK_BYTES;
                }

                auto direction = static_cast<ThroughputDirection>(directionRaw);
                bool validDirection = direction == ThroughputDirection::DOWNLOAD || direction == ThroughputDirection::UPLOAD;

                std::vector<uint8_t> ackBody;
                appendLe<uint8_t>(ackBody, static_cast<uint8_t>(validDirection ? 1 : 0));
                appendLe<uint8_t>(ackBody, 0);
                appendLe<uint16_t>(ackBody, 0);
                appendLe<uint32_t>(ackBody, durationMs);
                appendLe<uint32_t>(ackBody, chunkBytes);
                appendLe<uint8_t>(ackBody, static_cast<uint8_t>(serverLink.type));
                appendLe<uint8_t>(ackBody, 0);
                appendLe<uint16_t>(ackBody, 0);
                appendLe<uint32_t>(ackBody, serverLink.downMbps);
                appendLe<uint32_t>(ackBody, serverLink.upMbps);
                auto ack = makeTcpFrame(TcpMessageType::START_ACK, startHeader.sessionId, ackBody);
                if (!writeAll(clientFd, ack.data(), ack.size())) {
                    finish();
                    return;
                }
                if (!validDirection) {
                    finish();
                    return;
                }

                logger.log(LogLevel::SUMMARY,
                           "session_start",
                           "\"transport\":\"tcp\",\"sessionId\":" + std::to_string(startHeader.sessionId) +
                               ",\"client\":\"" + jsonEscape(addrToString(client)) +
                               "\",\"direction\":\"" + std::string(direction == ThroughputDirection::DOWNLOAD ? "download" : "upload") +
                               "\",\"durationMs\":" + std::to_string(durationMs) +
                               ",\"chunkBytes\":" + std::to_string(chunkBytes) +
                               ",\"serverIface\":\"" + jsonEscape(serverLink.iface) + "\"" +
                               ",\"serverLinkType\":\"" + jsonEscape(serverLinkTypeToString(serverLink.type)) + "\"" +
                               ",\"serverLinkDownMbps\":" + std::to_string(serverLink.downMbps) +
                               ",\"serverLinkUpMbps\":" + std::to_string(serverLink.upMbps));

                const uint64_t startNs = nowNs();
                uint64_t transferredBytes = 0;

                if (direction == ThroughputDirection::DOWNLOAD) {
                    std::vector<uint8_t> payload(chunkBytes);
                    uint8_t seed = static_cast<uint8_t>(startHeader.sessionId & 0xFF);
                    for (uint32_t i = 0; i < chunkBytes; ++i) {
                        payload[i] = static_cast<uint8_t>(seed + i);
                    }

                    const uint64_t deadlineNs = startNs + static_cast<uint64_t>(durationMs) * 1000000ULL;
                    while (running.load() && nowNs() < deadlineNs) {
                        auto dataFrame = makeTcpFrame(TcpMessageType::DATA, startHeader.sessionId, payload);
                        if (!writeAll(clientFd, dataFrame.data(), dataFrame.size())) {
                            break;
                        }
                        transferredBytes += payload.size();
                        counters.tcpBytesOut.fetch_add(payload.size());
                    }
                } else {
                    const uint64_t deadlineNs = startNs + static_cast<uint64_t>(durationMs) * 1000000ULL;
                    while (running.load() && nowNs() < deadlineNs) {
                        TcpHeader frameHeader{};
                        std::vector<uint8_t> frameBody;
                        if (!readTcpFrame(clientFd, frameHeader, frameBody)) {
                            continue;
                        }
                        if (frameHeader.sessionId != startHeader.sessionId) {
                            continue;
                        }
                        if (frameHeader.type == TcpMessageType::DATA) {
                            transferredBytes += frameBody.size();
                            counters.tcpBytesIn.fetch_add(frameBody.size());
                        } else if (frameHeader.type == TcpMessageType::STOP) {
                            break;
                        }
                    }
                }

                const uint64_t endNs = nowNs();
                const uint64_t durationNs = endNs > startNs ? (endNs - startNs) : 1ULL;

                std::vector<uint8_t> resultBody;
                appendLe<uint64_t>(resultBody, transferredBytes);
                appendLe<uint64_t>(resultBody, durationNs);
                auto resultFrame = makeTcpFrame(TcpMessageType::RESULT, startHeader.sessionId, resultBody);
                writeAll(clientFd, resultFrame.data(), resultFrame.size());

                logger.log(LogLevel::SUMMARY,
                           "session_end",
                           "\"transport\":\"tcp\",\"sessionId\":" + std::to_string(startHeader.sessionId) +
                               ",\"client\":\"" + jsonEscape(addrToString(client)) +
                               "\",\"bytes\":" + std::to_string(transferredBytes) +
                               ",\"durationNs\":" + std::to_string(durationNs));

                finish();
            }).detach();
        }
    });

    std::cout << "SpeedTestGamer server running on port " << options.port
              << " (UDP v2 + TCP throughput), maxSessions=" << options.maxSessions
              << ", iface=" << (serverLink.iface.empty() ? "n/a" : serverLink.iface)
              << ", link=" << serverLinkTypeToString(serverLink.type)
              << ", theoretical=" << serverLink.downMbps << "/" << serverLink.upMbps << " Mbps"
              << std::endl;

    uint64_t lastCleanupNs = nowNs();

    while (running.load()) {
        bool anyPacket = false;
        while (true) {
            uint8_t buffer[UDP_MAX_DATAGRAM_BYTES];
            sockaddr_in client{};
            socklen_t clientLen = sizeof(client);
            ssize_t n = recvfrom(udpFd,
                                 buffer,
                                 sizeof(buffer),
                                 0,
                                 reinterpret_cast<sockaddr*>(&client),
                                 &clientLen);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                perror("recvfrom UDP");
                break;
            }
            anyPacket = true;
            counters.udpPacketsIn.fetch_add(1);

            if (n < 12) {
                continue;
            }

            UdpHeader header{};
            if (!parseUdpHeader(buffer, static_cast<size_t>(n), header)) {
                continue;
            }
            if (header.version != UDP_PROTOCOL_VERSION) {
                continue;
            }

            const uint8_t* body = buffer + 12;
            const size_t bodySize = static_cast<size_t>(n) - 12;

            UdpSessionKey key{header.sessionId, client.sin_addr.s_addr, client.sin_port};

            if (header.type == UdpMessageType::SYNC_REQ) {
                size_t offset = 0;
                uint64_t clientSendNs = 0;
                if (!readLe<uint64_t>(body, bodySize, offset, clientSendNs) || offset != bodySize) {
                    continue;
                }

                const uint64_t recvNs = nowNs();
                const uint64_t sendNs = nowNs();
                std::vector<uint8_t> responseBody;
                appendLe<uint64_t>(responseBody, clientSendNs);
                appendLe<uint64_t>(responseBody, recvNs);
                appendLe<uint64_t>(responseBody, sendNs);
                auto packet = makeUdpPacket(UdpMessageType::SYNC_RESP, header.sessionId, header.seq, responseBody);
                sendto(udpFd, packet.data(), packet.size(), 0, reinterpret_cast<sockaddr*>(&client), clientLen);
                counters.udpPacketsOut.fetch_add(1);
                continue;
            }

            if (header.type == UdpMessageType::TEST_START_REQ) {
                size_t offset = 0;
                uint8_t runMode = 0;
                uint8_t pad = 0;
                uint16_t pad16 = 0;
                uint32_t tickMs = 0;
                uint32_t durationMs = 0;
                uint32_t packetCount = 0;
                uint32_t payloadUpBytes = 0;
                uint32_t payloadDownBytes = 0;

                if (!readLe<uint8_t>(body, bodySize, offset, runMode) ||
                    !readLe<uint8_t>(body, bodySize, offset, pad) ||
                    !readLe<uint16_t>(body, bodySize, offset, pad16) ||
                    !readLe<uint32_t>(body, bodySize, offset, tickMs) ||
                    !readLe<uint32_t>(body, bodySize, offset, durationMs) ||
                    !readLe<uint32_t>(body, bodySize, offset, packetCount) ||
                    !readLe<uint32_t>(body, bodySize, offset, payloadUpBytes) ||
                    !readLe<uint32_t>(body, bodySize, offset, payloadDownBytes) ||
                    offset != bodySize) {
                    continue;
                }

                bool accepted = true;
                uint32_t acceptedTick = options.tickOverrideMs > 0 ? static_cast<uint32_t>(options.tickOverrideMs) : tickMs;
                if (acceptedTick < UDP_MIN_TICK_MS || acceptedTick > UDP_MAX_TICK_MS) {
                    accepted = false;
                }

                if (payloadUpBytes > UDP_MAX_PAYLOAD_BYTES || payloadDownBytes > UDP_MAX_PAYLOAD_BYTES) {
                    accepted = false;
                }

                uint32_t resolvedCount = packetCount;
                if (runMode == 0) {
                    if (durationMs < acceptedTick) {
                        durationMs = acceptedTick;
                    }
                    resolvedCount = static_cast<uint32_t>(
                        std::ceil(static_cast<double>(durationMs) / static_cast<double>(acceptedTick)));
                }
                if (resolvedCount == 0 || resolvedCount > UDP_MAX_PACKET_COUNT) {
                    accepted = false;
                }

                {
                    std::lock_guard<std::mutex> lock(udpMutex);
                    bool alreadyExists = udpSessions.find(key) != udpSessions.end();
                    if (accepted && !alreadyExists && activeSessions.load() >= options.maxSessions) {
                        accepted = false;
                    }

                    if (accepted) {
                        UdpSession& session = udpSessions[key];
                        if (!alreadyExists) {
                            activeSessions.fetch_add(1);
                        }
                        session.sessionId = header.sessionId;
                        session.client = client;
                        session.tickMs = acceptedTick;
                        session.expectedCount = resolvedCount;
                        session.payloadUpBytes = payloadUpBytes;
                        session.payloadDownBytes = payloadDownBytes;
                        session.upReceivedCount = 0;
                        session.downSentCount = 0;
                        session.upOutOfOrderCount = 0;
                        session.maxSeqSeen = -1;
                        session.upBitmap.assign((resolvedCount + 7U) / 8U, 0);
                        session.startedNs = nowNs();
                        session.lastActivityNs = session.startedNs;

                        logger.log(LogLevel::SUMMARY,
                                   "session_start",
                                   "\"transport\":\"udp\",\"session\":\"" + jsonEscape(safeSessionTag(header.sessionId, client)) +
                                       "\",\"tickMs\":" + std::to_string(acceptedTick) +
                                       ",\"resolvedCount\":" + std::to_string(resolvedCount) +
                                       ",\"payloadUp\":" + std::to_string(payloadUpBytes) +
                                       ",\"payloadDown\":" + std::to_string(payloadDownBytes));
                    }
                }

                std::vector<uint8_t> ackBody;
                appendLe<uint32_t>(ackBody, acceptedTick);
                appendLe<uint32_t>(ackBody, resolvedCount);
                appendLe<uint32_t>(ackBody, payloadUpBytes);
                appendLe<uint32_t>(ackBody, payloadDownBytes);
                appendLe<uint8_t>(ackBody, static_cast<uint8_t>(accepted ? 1 : 0));
                appendLe<uint8_t>(ackBody, 0);
                appendLe<uint8_t>(ackBody, 0);
                appendLe<uint8_t>(ackBody, 0);
                auto ack = makeUdpPacket(UdpMessageType::TEST_START_ACK, header.sessionId, header.seq, ackBody);
                sendto(udpFd, ack.data(), ack.size(), 0, reinterpret_cast<sockaddr*>(&client), clientLen);
                counters.udpPacketsOut.fetch_add(1);
                continue;
            }

            if (header.type == UdpMessageType::UP_TICK) {
                size_t offset = 0;
                uint64_t clientSendNs = 0;
                uint32_t payloadSize = 0;
                if (!readLe<uint64_t>(body, bodySize, offset, clientSendNs) ||
                    !readLe<uint32_t>(body, bodySize, offset, payloadSize) ||
                    payloadSize > UDP_MAX_PAYLOAD_BYTES ||
                    offset + payloadSize != bodySize) {
                    continue;
                }

                UdpSession snapshot;
                bool hasSession = false;
                uint32_t flags = 0;
                uint64_t recvNs = nowNs();

                {
                    std::lock_guard<std::mutex> lock(udpMutex);
                    auto it = udpSessions.find(key);
                    if (it == udpSessions.end()) {
                        continue;
                    }
                    UdpSession& session = it->second;
                    session.lastActivityNs = recvNs;
                    uint32_t seq = header.seq;
                    if (seq < session.expectedCount) {
                        const size_t byteIndex = seq / 8U;
                        const uint8_t bit = static_cast<uint8_t>(1U << (seq % 8U));
                        if ((session.upBitmap[byteIndex] & bit) == 0) {
                            session.upBitmap[byteIndex] |= bit;
                            session.upReceivedCount += 1;
                        }

                        const bool outOfOrder = session.maxSeqSeen >= 0 && static_cast<int64_t>(seq) < session.maxSeqSeen;
                        if (outOfOrder) {
                            session.upOutOfOrderCount += 1;
                            flags |= 0x1U;
                        }
                        if (static_cast<int64_t>(seq) > session.maxSeqSeen) {
                            session.maxSeqSeen = static_cast<int64_t>(seq);
                        }
                        session.downSentCount += 1;
                        snapshot = session;
                        hasSession = true;
                    }
                }

                if (!hasSession) {
                    continue;
                }

                std::vector<uint8_t> downPayload(snapshot.payloadDownBytes);
                std::fill(downPayload.begin(), downPayload.end(), static_cast<uint8_t>(header.seq & 0xFF));

                const uint64_t sendNs = nowNs();
                std::vector<uint8_t> downBody;
                appendLe<uint64_t>(downBody, clientSendNs);
                appendLe<uint64_t>(downBody, recvNs);
                appendLe<uint64_t>(downBody, sendNs);
                appendLe<uint32_t>(downBody, flags);
                appendLe<uint32_t>(downBody, static_cast<uint32_t>(downPayload.size()));
                downBody.insert(downBody.end(), downPayload.begin(), downPayload.end());

                auto down = makeUdpPacket(UdpMessageType::DOWN_TICK, header.sessionId, header.seq, downBody);
                sendto(udpFd, down.data(), down.size(), 0, reinterpret_cast<sockaddr*>(&client), clientLen);
                counters.udpPacketsOut.fetch_add(1);
                continue;
            }

            if (header.type == UdpMessageType::TEST_END_REQ) {
                UdpSession session;
                bool hasSession = false;

                {
                    std::lock_guard<std::mutex> lock(udpMutex);
                    auto it = udpSessions.find(key);
                    if (it == udpSessions.end()) {
                        continue;
                    }
                    session = it->second;
                    hasSession = true;
                }

                if (!hasSession) {
                    continue;
                }

                std::vector<uint8_t> summaryBody;
                appendLe<uint32_t>(summaryBody, session.expectedCount);
                appendLe<uint32_t>(summaryBody, session.upReceivedCount);
                appendLe<uint32_t>(summaryBody, session.downSentCount);
                appendLe<uint32_t>(summaryBody, session.upOutOfOrderCount);
                appendLe<uint32_t>(summaryBody, static_cast<uint32_t>(session.upBitmap.size()));
                summaryBody.insert(summaryBody.end(), session.upBitmap.begin(), session.upBitmap.end());

                auto summary = makeUdpPacket(UdpMessageType::TEST_END_SUMMARY, header.sessionId, header.seq, summaryBody);
                sendto(udpFd, summary.data(), summary.size(), 0, reinterpret_cast<sockaddr*>(&client), clientLen);
                counters.udpPacketsOut.fetch_add(1);

                removeUdpSession(key, "client_end");
                continue;
            }
        }

        const uint64_t now = nowNs();
        if (now - lastCleanupNs >= 1000000000ULL) {
            lastCleanupNs = now;
            std::vector<UdpSessionKey> toRemove;
            {
                std::lock_guard<std::mutex> lock(udpMutex);
                for (const auto& entry : udpSessions) {
                    const UdpSession& session = entry.second;
                    uint64_t idleMs = (now > session.lastActivityNs) ? (now - session.lastActivityNs) / 1000000ULL : 0;
                    if (idleMs > static_cast<uint64_t>(SESSION_IDLE_TIMEOUT_MS)) {
                        toRemove.push_back(entry.first);
                    }
                }
            }
            for (const auto& key : toRemove) {
                removeUdpSession(key, "idle_timeout");
            }
        }

        if (!anyPacket) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    running.store(false);
    close(tcpFd);
    close(udpFd);

    if (tcpAcceptThread.joinable()) {
        tcpAcceptThread.join();
    }
    if (statsThread.joinable()) {
        statsThread.join();
    }

    logger.log(LogLevel::SUMMARY, "server_stop", "");
    return 0;
}
