#include "server/http_server.hpp"
#include "utils/logger.hpp"
#include "utils/path_utils.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace pxe {
namespace {

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string contentType(const std::string& path) {
    const std::string ext = toLower(std::filesystem::path(path).extension().string());
    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".json") return "application/json";
    if (ext == ".cfg" || ext == ".txt" || ext == ".md") return "text/plain; charset=utf-8";
    if (ext == ".iso") return "application/x-iso9660-image";
    if (ext == ".img" || ext == ".bin" || ext == ".0" || ext == ".c32" ||
        ext == ".efi" || ext == ".cat" || ext == ".lkrn") return "application/octet-stream";
    return "application/octet-stream";
}

int urlDecodePath(const std::string& raw, std::string& out) {
    out.clear();
    for (size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (c == '%' && i + 2 < raw.size()) {
            auto hex = [](char h) -> int {
                if (h >= '0' && h <= '9') return h - '0';
                if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                return -1;
            };
            const int hi = hex(raw[i + 1]);
            const int lo = hex(raw[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += c;
    }
    return 0;
}

bool sendAll(socket_t client, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        const int n = ::send(client, data + sent,
                             static_cast<int>(len - sent > 64 * 1024 ? 64 * 1024 : len - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool sendAll(socket_t client, const std::string& data) {
    return sendAll(client, data.data(), data.size());
}

void sendResponse(socket_t client, int code, const std::string& reason,
                  const std::string& type, const std::string& body, bool headOnly) {
    std::ostringstream out;
    out << "HTTP/1.1 " << code << ' ' << reason << "\r\n"
        << "Server: pxe-server\r\n"
        << "Content-Type: " << type << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n"
        << "\r\n";
    if (!sendAll(client, out.str())) return;
    if (!headOnly && !body.empty()) {
        sendAll(client, body);
    }
}

std::string trimCopy(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t");
    if (begin == std::string::npos) return std::string();
    const auto end = s.find_last_not_of(" \t");
    return s.substr(begin, end - begin + 1);
}

/*
 * Parses a single "Range: bytes=<first>-<last>" header.
 *
 * iPXE's `sanboot` mounts an ISO over HTTP by reading 2 KiB sectors, so it
 * needs 206 Partial Content replies; without range support HTTP ISO boot
 * cannot work at all. Multi-range requests are answered with the first range.
 */
bool parseRangeHeader(const std::string& request, uint64_t fileSize, bool& hasRange,
                      uint64_t& first, uint64_t& last, bool& unsatisfiable) {
    hasRange = false;
    unsatisfiable = false;

    std::istringstream in(request);
    std::string line;
    std::getline(in, line);  // request line
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        if (toLower(trimCopy(line.substr(0, colon))) != "range") continue;

        const std::string value = trimCopy(line.substr(colon + 1));
        if (toLower(value.substr(0, 6)) != "bytes=") return false;

        std::string spec = value.substr(6);
        const auto comma = spec.find(',');
        if (comma != std::string::npos) spec = spec.substr(0, comma);
        const auto dash = spec.find('-');
        if (dash == std::string::npos) return false;

        const std::string startText = trimCopy(spec.substr(0, dash));
        const std::string endText = trimCopy(spec.substr(dash + 1));
        if (fileSize == 0) {
            unsatisfiable = true;
            return false;
        }

        auto toNumber = [](const std::string& text, uint64_t& out) {
            if (text.empty()) return false;
            out = 0;
            for (char c : text) {
                if (c < '0' || c > '9') return false;
                out = out * 10 + static_cast<uint64_t>(c - '0');
            }
            return true;
        };

        if (startText.empty()) {
            uint64_t suffix = 0;
            if (!toNumber(endText, suffix) || suffix == 0) {
                unsatisfiable = true;
                return false;
            }
            first = suffix >= fileSize ? 0 : fileSize - suffix;
            last = fileSize - 1;
        } else {
            uint64_t startValue = 0;
            uint64_t endValue = 0;
            if (!toNumber(startText, startValue)) return false;
            if (endText.empty()) endValue = fileSize - 1;
            else if (!toNumber(endText, endValue)) return false;
            if (startValue >= fileSize) {
                unsatisfiable = true;
                return false;
            }
            if (endValue >= fileSize) endValue = fileSize - 1;
            if (startValue > endValue) {
                unsatisfiable = true;
                return false;
            }
            first = startValue;
            last = endValue;
        }

        hasRange = true;
        return true;
    }
    return false;
}

void sendRangeNotSatisfiable(socket_t client, uint64_t fileSize) {
    std::ostringstream out;
    out << "HTTP/1.1 416 Range Not Satisfiable\r\n"
        << "Server: pxe-server\r\n"
        << "Content-Range: bytes */" << fileSize << "\r\n"
        << "Content-Length: 0\r\n"
        << "Connection: close\r\n"
        << "\r\n";
    sendAll(client, out.str());
}

void sendFileResponse(socket_t client, const std::string& type, std::ifstream& file,
                      uint64_t fileSize, bool headOnly, bool hasRange,
                      uint64_t rangeStart, uint64_t rangeEnd) {
    const uint64_t count = hasRange ? (rangeEnd - rangeStart + 1) : fileSize;

    std::ostringstream out;
    out << (hasRange ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n")
        << "Server: pxe-server\r\n"
        << "Content-Type: " << type << "\r\n"
        << "Content-Length: " << count << "\r\n"
        << "Accept-Ranges: bytes\r\n";
    if (hasRange) {
        out << "Content-Range: bytes " << rangeStart << '-' << rangeEnd
            << '/' << fileSize << "\r\n";
    }
    out << "Connection: close\r\n"
        << "\r\n";
    if (!sendAll(client, out.str()) || headOnly) return;

    if (hasRange) file.seekg(static_cast<std::streamoff>(rangeStart), std::ios::beg);

    std::vector<char> buf(1024 * 1024);
    uint64_t remaining = count;
    while (remaining > 0 && file.good()) {
        const std::streamsize toRead = static_cast<std::streamsize>(
            remaining < buf.size() ? remaining : buf.size());
        file.read(buf.data(), toRead);
        const std::streamsize got = file.gcount();
        if (got <= 0) break;
        if (!sendAll(client, buf.data(), static_cast<size_t>(got))) return;
        remaining -= static_cast<uint64_t>(got);
    }
}

} // namespace

HTTPServer::HTTPServer(const Config& cfg) : cfg_(cfg) {
    started_ = std::chrono::steady_clock::now();
}

HTTPServer::~HTTPServer() {
    stop();
}

std::string HTTPServer::statusJson() const {
    const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - started_)
                            .count();
    std::error_code ec;
    bool bootPresent = false;
    if (cfg_.http_boot) {
        bootPresent = true; // URL-based boot; reachability checked at request time
    } else if (!cfg_.boot_file.empty()) {
        bootPresent = std::filesystem::exists(std::filesystem::path(cfg_.boot_root) / cfg_.boot_file, ec);
    }
    std::ostringstream out;
    out << "{"
        << "\"server\":\"pxe-server\","
        << "\"uptime_seconds\":" << uptime << ","
        << "\"server_ip\":\"" << cfg_.server_ip << "\","
        << "\"dhcp_port\":" << cfg_.dhcp_port << ","
        << "\"tftp_port\":" << cfg_.tftp_port << ","
        << "\"http_port\":" << cfg_.http_port << ","
        << "\"ip_range\":\"" << cfg_.range_start << "-" << cfg_.range_end << "\","
        << "\"boot_root\":\"" << cfg_.boot_root << "\","
        << "\"http_root\":\"" << cfg_.http_root << "\","
        << "\"boot_file\":\"" << cfg_.boot_file << "\","
        << "\"http_boot\":" << (cfg_.http_boot ? "true" : "false") << ","
        << "\"boot_file_present\":" << (bootPresent ? "true" : "false")
        << "}";
    return out.str();
}

std::string HTTPServer::indexHtml() const {
    std::ostringstream out;
    out << "<!doctype html><html><head><meta charset=\"utf-8\">"
        << "<title>PXE Server</title>"
        << "<style>body{font-family:system-ui,sans-serif;margin:2rem;}"
        << "code{background:#eee;padding:.1rem .3rem;border-radius:3px;}"
        << "li{margin:.2rem 0}</style></head><body>"
        << "<h1>PXE Server</h1>"
        << "<p>TFTP root: <code>" << cfg_.boot_root << "</code><br>"
        << "HTTP root: <code>" << cfg_.http_root << "</code><br>"
        << "DHCP port: <code>" << cfg_.dhcp_port << "</code> &middot; "
        << "TFTP port: <code>" << cfg_.tftp_port << "</code> &middot; "
        << "HTTP port: <code>" << cfg_.http_port << "</code><br>"
        << "Boot file: <code>" << cfg_.boot_file << "</code> &middot; "
        << "<a href=\"/status\">/status</a></p><h2>HTTP root</h2><ul>";

    std::error_code ec;
    const auto& listRoot = cfg_.http_root.empty() ? cfg_.boot_root : cfg_.http_root;
    if (std::filesystem::exists(listRoot, ec)) {
        std::vector<std::string> entries;
        for (const auto& entry : std::filesystem::directory_iterator(listRoot, ec)) {
            entries.push_back(entry.path().filename().string() +
                              (entry.is_directory() ? "/" : ""));
        }
        std::sort(entries.begin(), entries.end());
        for (const auto& name : entries) {
            out << "<li>" << name << "</li>";
        }
    }
    out << "</ul></body></html>";
    return out.str();
}

void HTTPServer::handleClient(socket_t client, const struct sockaddr_in& from) {
    socketSetRecvTimeout(client, 5000);
    socketSetSendTimeout(client, 10000);

    char buffer[8192];
    const int n = ::recv(client, buffer, sizeof(buffer) - 1, 0);
    if (n <= 0) return;
    buffer[n] = '\0';

    const std::string requestText(buffer, static_cast<size_t>(n));
    std::istringstream request(requestText);
    std::string method;
    std::string target;
    std::string version;
    request >> method >> target >> version;
    if (method.empty() || target.empty()) return;

    const bool headOnly = (method == "HEAD");
    const std::string fromIp = sockaddrToIp(from);

    if (method != "GET" && method != "HEAD") {
        Logger::instance().warn("HTTP " + method + " " + target + " from " + fromIp + " -> 405 method not allowed");
        sendResponse(client, 405, "Method Not Allowed", "text/plain", "method not allowed\n", false);
        return;
    }

    std::string path = target;
    const auto query = path.find('?');
    if (query != std::string::npos) path = path.substr(0, query);
    std::string decoded;
    urlDecodePath(path, decoded);

    Logger::instance().info("HTTP req " + method + " target=\"" + target + "\" decoded=\"" + decoded +
                            "\" from " + fromIp);

    if (decoded == "/status") {
        Logger::instance().info("HTTP " + method + " /status from " + fromIp + " -> 200 json");
        sendResponse(client, 200, "OK", "application/json", statusJson(), headOnly);
        return;
    }
    if (decoded == "/" || decoded.empty()) {
        Logger::instance().info("HTTP " + method + " / from " + fromIp + " -> 200 index");
        sendResponse(client, 200, "OK", "text/html; charset=utf-8", indexHtml(), headOnly);
        return;
    }

    std::string relative = decoded;
    while (!relative.empty() && relative.front() == '/') relative.erase(relative.begin());
    if (relative.find("..") != std::string::npos) {
        Logger::instance().warn("HTTP " + method + " " + decoded + " from " + fromIp +
                                " -> 403 path traversal blocked (relative=\"" + relative + "\")");
        sendResponse(client, 403, "Forbidden", "text/plain", "forbidden\n", headOnly);
        return;
    }

    std::error_code ec;
    const std::string fullPathStr = pathutil::join(cfg_.http_root, relative);
    const std::filesystem::path filePath(fullPathStr);
    const bool exists = std::filesystem::exists(filePath, ec);
    const bool isDir = exists && std::filesystem::is_directory(filePath, ec);
    Logger::instance().info("HTTP resolve: http_root=\"" + pathutil::normalizeFsPath(cfg_.http_root) + "\" relative=\"" + relative +
                            "\" full=\"" + pathutil::normalizeFsPath(fullPathStr) + "\" exists=" + (exists ? "yes" : "no") +
                            " dir=" + (isDir ? "yes" : "no"));

    if (!exists || isDir) {
        Logger::instance().warn("HTTP " + method + " " + decoded + " from " + fromIp +
                                " -> 404 not found (full=\"" + pathutil::normalizeFsPath(fullPathStr) + "\")");
        sendResponse(client, 404, "Not Found", "text/plain", "not found\n", headOnly);
        return;
    }

    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        Logger::instance().error("HTTP " + method + " " + decoded + " from " + fromIp +
                                 " -> 500 cannot open file (full=\"" + pathutil::normalizeFsPath(fullPathStr) + "\")");
        sendResponse(client, 500, "Internal Server Error", "text/plain", "cannot read file\n", headOnly);
        return;
    }
    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    file.seekg(0, std::ios::beg);
    if (fileSize < 0) {
        Logger::instance().error("HTTP " + method + " " + decoded + " from " + fromIp +
                                 " -> 500 cannot stat file size (full=\"" + pathutil::normalizeFsPath(fullPathStr) + "\")");
        sendResponse(client, 500, "Internal Server Error", "text/plain", "cannot read file\n", headOnly);
        return;
    }

    const auto type = contentType(fullPathStr);

    // iPXE sanboot reads an ISO in small ranged chunks; a 206 reply is what
    // makes "boot an ISO over HTTP" work.
    bool hasRange = false;
    bool unsatisfiable = false;
    uint64_t rangeStart = 0;
    uint64_t rangeEnd = 0;
    parseRangeHeader(requestText, static_cast<uint64_t>(fileSize), hasRange,
                     rangeStart, rangeEnd, unsatisfiable);
    if (unsatisfiable) {
        Logger::instance().warn("HTTP " + method + " " + decoded + " from " + fromIp +
                                " -> 416 range not satisfiable (size=" +
                                std::to_string(static_cast<long long>(fileSize)) + ")");
        sendRangeNotSatisfiable(client, static_cast<uint64_t>(fileSize));
        return;
    }

    Logger::instance().info("HTTP " + method + " " + decoded + " from " + fromIp +
                            " -> " + (hasRange ? "206 partial" : "200 start") + " (full=\"" +
                            pathutil::normalizeFsPath(fullPathStr) + "\", size=" +
                            std::to_string(static_cast<long long>(fileSize)) +
                            " bytes, type=" + type + (headOnly ? ", head" : "") +
                            (hasRange ? ", range=" + std::to_string(static_cast<long long>(rangeStart)) +
                                            "-" + std::to_string(static_cast<long long>(rangeEnd))
                                      : std::string()) +
                            ")");

    sendFileResponse(client, type, file, static_cast<uint64_t>(fileSize), headOnly,
                     hasRange, rangeStart, rangeEnd);

    Logger::instance().info("HTTP " + method + " " + decoded + " from " + fromIp +
                            " -> " + (hasRange ? "206" : "200") + " done (full=\"" +
                            pathutil::normalizeFsPath(fullPathStr) + "\")");
}

void HTTPServer::loop() {
    while (running_.load()) {
        struct sockaddr_in from;
        std::memset(&from, 0, sizeof(from));
        socklen_type fromLen = sizeof(from);
        const socket_t client = ::accept(sock_, reinterpret_cast<struct sockaddr*>(&from), &fromLen);
        if (!socketValid(client)) {
            if (!running_.load()) break;
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(handlersMutex_);
            ++handlers_;
        }
        std::thread([this, client, from]() {
            handleClient(client, from);
            socketGracefulClose(client);
            std::lock_guard<std::mutex> lock(handlersMutex_);
            --handlers_;
            handlersCv_.notify_all();
        }).detach();
    }
}

bool HTTPServer::start() {
    if (running_.load()) return true;
    if (cfg_.http_port <= 0) {
        Logger::instance().info("HTTP disabled (port 0)");
        return true;
    }

    sock_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (!socketValid(sock_)) {
        error_ = "socket() failed";
        Logger::instance().error("HTTP " + error_);
        return false;
    }
    socketSetReuse(sock_);

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(cfg_.http_port));
    ::inet_pton(AF_INET, cfg_.host == "0.0.0.0" ? "0.0.0.0" : cfg_.host.c_str(), &addr.sin_addr);

    if (::bind(sock_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        error_ = "bind failed on port " + std::to_string(cfg_.http_port) +
                 " (error " + std::to_string(socketLastError()) + ")";
        Logger::instance().error("HTTP " + error_);
        socketClose(sock_);
        sock_ = kInvalidSocket;
        return false;
    }
    if (::listen(sock_, 16) != 0) {
        error_ = "listen failed (error " + std::to_string(socketLastError()) + ")";
        Logger::instance().error("HTTP " + error_);
        socketClose(sock_);
        sock_ = kInvalidSocket;
        return false;
    }

    socketSetRecvTimeout(sock_, 500);
    running_.store(true);
    thread_ = std::thread(&HTTPServer::loop, this);

    Logger::instance().info("HTTP serving " + cfg_.http_root + " on " + cfg_.host + ":" +
                            std::to_string(cfg_.http_port));
    return true;
}

void HTTPServer::stop() {
    if (running_.exchange(false)) {
        if (socketValid(sock_)) {
            socketClose(sock_);
            sock_ = kInvalidSocket;
        }
        if (thread_.joinable()) thread_.join();
        // Drain in-flight client handlers before tearing anything down.
        std::unique_lock<std::mutex> lock(handlersMutex_);
        handlersCv_.wait(lock, [this] { return handlers_ == 0; });
        Logger::instance().info("HTTP stopped");
    } else if (socketValid(sock_)) {
        socketClose(sock_);
        sock_ = kInvalidSocket;
    }
}

} // namespace pxe
