#include "server/tftp_server.hpp"
#include "utils/logger.hpp"
#include "utils/path_utils.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace pxe {
namespace {

enum : uint16_t {
    OP_RRQ = 1,
    OP_WRQ = 2,
    OP_DATA = 3,
    OP_ACK = 4,
    OP_ERROR = 5,
    OP_OACK = 6
};

enum : uint16_t {
    ERR_UNDEFINED = 0,
    ERR_NOT_FOUND = 1,
    ERR_ACCESS = 2,
    ERR_DISK_FULL = 3,
    ERR_ILLEGAL_OP = 4,
    ERR_UNKNOWN_TID = 5,
    ERR_EXISTS = 6,
    ERR_NO_USER = 7
};

std::vector<uint8_t> errorPacket(uint16_t code, const std::string& message) {
    std::vector<uint8_t> out;
    out.push_back(0);
    out.push_back(static_cast<uint8_t>(OP_ERROR));
    out.push_back(static_cast<uint8_t>(code >> 8));
    out.push_back(static_cast<uint8_t>(code & 0xFF));
    out.insert(out.end(), message.begin(), message.end());
    out.push_back(0);
    return out;
}

void sendError(socket_t sock, const struct sockaddr_in& to, uint16_t code, const std::string& msg) {
    const auto packet = errorPacket(code, msg);
    ::sendto(sock, reinterpret_cast<const char*>(packet.data()),
             static_cast<int>(packet.size()), 0,
             reinterpret_cast<const struct sockaddr*>(&to), sizeof(to));
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Identifies an in-flight transfer: client address + client port (TID) + file.
std::string transferKeyOf(const struct sockaddr_in& client, const std::string& filename) {
    return sockaddrToIp(client) + ":" + std::to_string(ntohs(client.sin_port)) + "|" + filename;
}

constexpr int kDefaultBlksize = 512;   // RFC 1350 default, used when nothing is negotiated
constexpr int kMaxBlksizeCap = 8192;   // keep a single datagram within a sane MTU budget
constexpr int kMaxTimeoutSec = 10;     // bound retransmit stalls so a PXE ROM cannot hang
constexpr int kOackAttempts = 5;
constexpr int kDataAttempts = 5;

} // namespace

TFTPServer::TFTPServer(const Config& cfg) : cfg_(cfg) {}

TFTPServer::~TFTPServer() {
    stop();
}

std::string TFTPServer::resolvePath(const std::string& filename) const {
    std::string clean = pathutil::normalizeSlashes(filename);
    while (!clean.empty() && clean.front() == '/') clean.erase(clean.begin());
    if (clean.empty() || clean.find("..") != std::string::npos) return std::string();
    return pathutil::join(cfg_.boot_root, clean);
}

void TFTPServer::transferFile(const struct sockaddr_in& client, const std::string& filename,
                              const std::map<std::string, std::string>& options,
                              const std::string& transferKey) {
    const std::string clientIp = sockaddrToIp(client);

    if (active_.load() >= cfg_.tftp_max_clients) {
        socket_t tmp = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (socketValid(tmp)) {
            sendError(tmp, client, ERR_UNDEFINED, "server busy");
            socketClose(tmp);
        }
        Logger::instance().warn("TFTP busy, rejecting request file=\"" + filename + "\" from " + clientIp);
        return;
    }
    active_.fetch_add(1);

    socket_t sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (!socketValid(sock)) {
        active_.fetch_sub(1);
        Logger::instance().error("TFTP cannot create transfer socket for file=\"" + filename + "\" from " + clientIp);
        return;
    }

    struct sockaddr_in local;
    std::memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = 0;
    if (::bind(sock, reinterpret_cast<struct sockaddr*>(&local), sizeof(local)) != 0) {
        Logger::instance().error("TFTP bind failed for transfer socket file=\"" + filename + "\" from " + clientIp +
                                 " err=" + std::to_string(socketLastError()));
        socketClose(sock);
        active_.fetch_sub(1);
        return;
    }
    // Connect so stray packets from other hosts are ignored.
    ::connect(sock, reinterpret_cast<const struct sockaddr*>(&client), sizeof(client));

    const std::string path = resolvePath(filename);
    std::error_code ec;
    const bool pathEmpty = path.empty();
    const bool exists = !pathEmpty && std::filesystem::exists(path, ec);
    const bool isDir = exists && std::filesystem::is_directory(path, ec);
    Logger::instance().info("TFTP resolve: req=\"" + filename + "\" boot_root=\"" + cfg_.boot_root +
                            "\" full=\"" + path + "\" exists=" + (exists ? "yes" : "no") +
                            " dir=" + (isDir ? "yes" : "no") + " from " + clientIp);

    std::ifstream file;
    if (!pathEmpty && exists && !isDir) {
        file.open(path, std::ios::binary);
    }
    if (pathEmpty || !exists || isDir || !file.is_open()) {
        std::string reason = "file not found";
        if (pathEmpty) reason = "invalid/empty resolved path";
        else if (!exists) reason = "path does not exist";
        else if (isDir) reason = "path is a directory";
        else reason = "cannot open file";
        sendError(sock, client, ERR_NOT_FOUND, reason + ": " + filename);
        Logger::instance().warn("TFTP reject: req=\"" + filename + "\" full=\"" + path + "\" from " + clientIp +
                                " reason=" + reason);
        socketClose(sock);
        active_.fetch_sub(1);
        return;
    }

    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    file.seekg(0, std::ios::beg);
    Logger::instance().info("TFTP open ok: req=\"" + filename + "\" full=\"" + path + "\" size=" +
                            std::to_string(static_cast<long long>(fileSize)) + " from " + clientIp);

    /*
     * RFC 1350: a client that sends a plain RRQ (no options) expects
     * 512-byte DATA blocks. Sending a configured non-default size (the old
     * code sent cfg blksize = 128) to such a client makes it parse the stream
     * with the wrong block size and the NBP comes out corrupt -> PXE-E77.
     */
    int blksize = kDefaultBlksize;

    // Retransmit interval. This must stay short: with the old (broken) Windows
    // timeout a single late ACK already killed the transfer.
    int timeoutSec = cfg_.tftp_timeout > 0 ? cfg_.tftp_timeout : 3;
    if (timeoutSec > kMaxTimeoutSec) timeoutSec = kMaxTimeoutSec;
    int timeoutMs = timeoutSec * 1000;

    struct AckOptions {
        bool blksize = false;
        bool tsize = false;
        bool timeout = false;
    } negotiated;

    for (const auto& option : options) {
        const std::string key = toLower(option.first);
        if (key == "blksize") {
            const int requested = std::atoi(option.second.c_str());
            if (requested >= 8) {
                // Honour the requested size (PXE ROMs ask for 1456); the
                // configured value only caps what we are willing to grant.
                int cap = cfg_.tftp_blksize >= 8 ? cfg_.tftp_blksize : kDefaultBlksize;
                if (cap > kMaxBlksizeCap) cap = kMaxBlksizeCap;
                blksize = std::min(requested, cap);
                negotiated.blksize = true;
            }
        } else if (key == "tsize") {
            negotiated.tsize = true;
        } else if (key == "timeout") {
            const int requested = std::atoi(option.second.c_str());
            if (requested >= 1) {
                timeoutSec = std::min(requested, kMaxTimeoutSec);
                negotiated.timeout = true;
            }
        }
    }
    timeoutMs = timeoutSec * 1000;

    socketSetRecvTimeout(sock, timeoutMs);

    uint16_t block = 1;

    if (negotiated.blksize || negotiated.tsize || negotiated.timeout) {
        Logger::instance().info("TFTP OACK for \"" + filename + "\" from " + clientIp +
                                " -> blksize=" + std::to_string(blksize) +
                                " timeout=" + std::to_string(timeoutSec) +
                                "s tsize=" + (negotiated.tsize ? std::to_string(static_cast<long long>(fileSize)) : "n/a"));
        std::vector<uint8_t> oack = {0, OP_OACK};
        auto appendOption = [&oack](const std::string& k, const std::string& v) {
            oack.insert(oack.end(), k.begin(), k.end());
            oack.push_back(0);
            oack.insert(oack.end(), v.begin(), v.end());
            oack.push_back(0);
        };
        if (negotiated.blksize) appendOption("blksize", std::to_string(blksize));
        if (negotiated.tsize) appendOption("tsize", std::to_string(fileSize));
        if (negotiated.timeout) appendOption("timeout", std::to_string(timeoutSec));

        bool acked = false;
        for (int attempt = 0; attempt < kOackAttempts && !acked; ++attempt) {
            ::send(sock, reinterpret_cast<const char*>(oack.data()),
                   static_cast<int>(oack.size()), 0);
            uint8_t reply[64];
            const int n = ::recv(sock, reinterpret_cast<char*>(reply), sizeof(reply), 0);
            if (n >= 4 && reply[1] == OP_ACK && reply[2] == 0 && reply[3] == 0) acked = true;
        }
        if (!acked) {
            /*
             * The client never accepted the OACK, so it is still using the
             * protocol defaults: 512-byte blocks and no options. Fall back to
             * exactly that - the previous code fell back to the configured
             * blksize (128), which the client then mis-parsed.
             */
            Logger::instance().warn("TFTP option negotiation failed for \"" + filename + "\" from " + clientIp +
                                    " - falling back to a plain 512-byte transfer");
            negotiated.blksize = false;
            negotiated.tsize = false;
            negotiated.timeout = false;
            blksize = kDefaultBlksize;
            timeoutSec = cfg_.tftp_timeout > 0 ? cfg_.tftp_timeout : 3;
            timeoutSec = std::max(1, std::min(timeoutSec, kMaxTimeoutSec));
            timeoutMs = timeoutSec * 1000;
            socketSetRecvTimeout(sock, timeoutMs);
        } else {
            Logger::instance().info("TFTP OACK accepted for \"" + filename + "\" from " + clientIp);
        }
        block = 1;
    } else {
        Logger::instance().info("TFTP transfer params for \"" + filename + "\" from " + clientIp +
                                " -> blksize=" + std::to_string(blksize) +
                                " timeout=" + std::to_string(timeoutSec) + "s (no OACK)");
    }

    std::vector<uint8_t> dataPacket(static_cast<size_t>(blksize) + 4);
    std::vector<char> chunk(static_cast<size_t>(blksize));

    size_t sentBytes = 0;
    bool ok = true;
    while (ok && running_.load()) {
        file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const std::streamsize got = file.gcount();

        dataPacket[0] = 0;
        dataPacket[1] = OP_DATA;
        dataPacket[2] = static_cast<uint8_t>(block >> 8);
        dataPacket[3] = static_cast<uint8_t>(block & 0xFF);
        std::memcpy(dataPacket.data() + 4, chunk.data(), static_cast<size_t>(got));

        const int packetLen = 4 + static_cast<int>(got);
        bool acked = false;
        for (int attempt = 0; attempt < kDataAttempts && !acked; ++attempt) {
            ::send(sock, reinterpret_cast<const char*>(dataPacket.data()), packetLen, 0);

            uint8_t reply[64];
            const int n = ::recv(sock, reinterpret_cast<char*>(reply), sizeof(reply), 0);
            if (n < 4) continue;
            const uint16_t opcode = static_cast<uint16_t>((reply[1]));
            const uint16_t ackBlock = static_cast<uint16_t>((reply[2] << 8) | reply[3]);
            if (opcode == OP_ACK && ackBlock == block) {
                acked = true;
            } else if (opcode == OP_ERROR) {
                ok = false;
                break;
            }
        }

        if (!acked) {
            Logger::instance().warn("TFTP timeout: file=\"" + filename + "\" from " + clientIp +
                                    " at block=" + std::to_string(block) +
                                    " sent_bytes=" + std::to_string(sentBytes) +
                                    " blksize=" + std::to_string(blksize));
            ok = false;
            break;
        }

        sentBytes += static_cast<size_t>(got);
        if (got < static_cast<std::streamsize>(blksize)) break;
        ++block;
    }

    if (ok) {
        Logger::instance().info("TFTP sent ok: file=\"" + filename + "\" to " + clientIp +
                                " bytes=" + std::to_string(sentBytes) +
                                " blksize=" + std::to_string(blksize) +
                                " last_block=" + std::to_string(block));
    } else {
        Logger::instance().warn("TFTP incomplete: file=\"" + filename + "\" to " + clientIp +
                                " bytes_sent=" + std::to_string(sentBytes) +
                                " last_block=" + std::to_string(block));
    }

    socketClose(sock);
    active_.fetch_sub(1);
}

void TFTPServer::serveRequest(const char* data, size_t len, const struct sockaddr_in& from) {
    if (len < 4) return;
    const uint16_t opcode = static_cast<uint16_t>((static_cast<uint8_t>(data[0]) << 8) |
                                                  static_cast<uint8_t>(data[1]));
    if (opcode != OP_RRQ) {
        socket_t tmp = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (socketValid(tmp)) {
            sendError(tmp, from, opcode == OP_WRQ ? ERR_ACCESS : ERR_ILLEGAL_OP,
                      opcode == OP_WRQ ? "server is read-only" : "unsupported operation");
            socketClose(tmp);
        }
        return;
    }

    // filename\0mode\0[option\0value\0]...
    std::vector<std::string> fields;
    std::string current;
    for (size_t i = 2; i < len && fields.size() < 12; ++i) {
        if (data[i] == '\0') {
            fields.push_back(current);
            current.clear();
        } else {
            current += data[i];
        }
    }
    if (fields.empty() || fields.front().empty()) return;

    const std::string filename = fields[0];
    std::map<std::string, std::string> options;
    for (size_t i = 2; i + 1 < fields.size(); i += 2) {
        options[fields[i]] = fields[i + 1];
    }

    {
        std::string opts;
        for (const auto& kv : options) {
            if (!opts.empty()) opts += ", ";
            opts += kv.first + "=" + kv.second;
        }
        Logger::instance().info("TFTP RRQ req=\"" + filename + "\" from " + sockaddrToIp(from) +
                                " opts=[" + opts + "]");
    }

    // One transfer per client TID + file (see transfers_ in the header).
    const std::string key = transferKeyOf(from, filename);
    {
        std::lock_guard<std::mutex> lock(transfersMutex_);
        const auto now = std::chrono::steady_clock::now();
        const auto it = transfers_.find(key);
        if (it != transfers_.end() &&
            std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count() < 15) {
            Logger::instance().debug("TFTP duplicate RRQ ignored: req=\"" + filename +
                                     "\" from " + sockaddrToIp(from));
            return;
        }
        transfers_[key] = now;
    }

    std::thread([this, from, filename, options, key]() {
        transferFile(from, filename, options, key);
        releaseTransfer(key);
    }).detach();
}

void TFTPServer::releaseTransfer(const std::string& transferKey) {
    std::lock_guard<std::mutex> lock(transfersMutex_);
    transfers_.erase(transferKey);
}

void TFTPServer::loop() {
    char buffer[1024];
    while (running_.load()) {
        struct sockaddr_in from;
        std::memset(&from, 0, sizeof(from));
        socklen_type fromLen = sizeof(from);

        const int n = ::recvfrom(sock_, buffer, sizeof(buffer), 0,
                                 reinterpret_cast<struct sockaddr*>(&from), &fromLen);
        if (n <= 0) {
            if (!running_.load()) break;
            continue;
        }
        serveRequest(buffer, static_cast<size_t>(n), from);
    }
}

bool TFTPServer::start() {
    if (running_.load()) return true;

    sock_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (!socketValid(sock_)) {
        error_ = "socket() failed";
        Logger::instance().error("TFTP " + error_);
        return false;
    }
    socketSetReuse(sock_);

    // Large buffers help sustained transfers to slow PXE ROMs.
    int bufSize = 1024 * 1024;
    ::setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bufSize), sizeof(bufSize));
    ::setsockopt(sock_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bufSize), sizeof(bufSize));

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(cfg_.tftp_port));
    struct in_addr bindAddr{};
    ::inet_pton(AF_INET, cfg_.host == "0.0.0.0" ? "0.0.0.0" : cfg_.host.c_str(), &bindAddr);
    addr.sin_addr = bindAddr;

    if (::bind(sock_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        error_ = "bind failed on port " + std::to_string(cfg_.tftp_port) +
                 " (error " + std::to_string(socketLastError()) + ")";
        Logger::instance().error("TFTP " + error_);
        socketClose(sock_);
        sock_ = kInvalidSocket;
        return false;
    }

    socketSetRecvTimeout(sock_, 500);
    running_.store(true);
    thread_ = std::thread(&TFTPServer::loop, this);

    std::error_code ec;
    const bool rootExists = std::filesystem::exists(cfg_.boot_root, ec);
    Logger::instance().info("TFTP serving boot_root=\"" + cfg_.boot_root + "\" exists=" + (rootExists ? "yes" : "no") +
                            " on " + cfg_.host + ":" + std::to_string(cfg_.tftp_port) +
                            " timeout=" + std::to_string(cfg_.tftp_timeout) +
                            "s blksize=" + std::to_string(cfg_.tftp_blksize) +
                            " max_clients=" + std::to_string(cfg_.tftp_max_clients));
    return true;
}

void TFTPServer::stop() {
    if (running_.exchange(false)) {
        if (socketValid(sock_)) {
            socketClose(sock_);
            sock_ = kInvalidSocket;
        }
        if (thread_.joinable()) thread_.join();
        // Transfers run on detached threads; let them notice the stop flag.
        for (int i = 0; i < 30 && active_.load() > 0; ++i) {
            sleepMs(100);
        }
        Logger::instance().info("TFTP stopped");
    } else if (socketValid(sock_)) {
        socketClose(sock_);
        sock_ = kInvalidSocket;
    }
}

} // namespace pxe
