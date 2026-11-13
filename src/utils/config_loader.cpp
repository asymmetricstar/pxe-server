#include "utils/config_loader.hpp"
#include "compat/net.hpp"
#include "utils/path_utils.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace pxe {
namespace {

std::string trim(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return std::string();
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

std::string unquote(const std::string& s) {
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                          (s.front() == '\'' && s.back() == '\''))) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

void replaceAll(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

} // namespace

std::string Config::get(const std::string& section, const std::string& key,
                        const std::string& def) const {
    const auto it = raw.find(toLower(section) + "." + toLower(key));
    if (it == raw.end() || it->second.empty()) return def;
    return it->second;
}

int Config::get_int(const std::string& section, const std::string& key, int def) const {
    const std::string value = get(section, key);
    if (value.empty()) return def;
    try {
        return std::stoi(value);
    } catch (...) {
        return def;
    }
}

std::string ConfigLoader::resolvePath(const std::string& path, const std::string& root) {
    if (path.empty()) return path;
    std::string out = path;
    const std::string normRoot = pathutil::normalizeFsPath(root);
    replaceAll(out, "${SERVER_ROOT}", normRoot);
    if (out.find("://") != std::string::npos) {
        return pathutil::normalizeSlashes(out);
    }
    return pathutil::normalizeFsPath(out);
}

Config ConfigLoader::load(const std::string& filename) {
    Config cfg;
    cfg.file = filename;

    std::ifstream in(filename);
    if (!in.is_open()) {
        cfg.error = "cannot open config file: " + filename;
        return cfg;
    }

    // ${SERVER_ROOT} is the directory above the config directory, i.e. the
    // distribution root (build/bin) when running from build/bin/config.
    std::error_code ec;
    std::filesystem::path cfg_path = std::filesystem::absolute(filename, ec);
    if (ec) cfg_path = std::filesystem::path(filename);
    std::filesystem::path cfg_dir = cfg_path.parent_path();
    std::filesystem::path root = cfg_dir;
    if (toLower(cfg_dir.filename().string()) == "config" && !cfg_dir.parent_path().empty()) {
        root = cfg_dir.parent_path();
    }
    if (root.empty()) root = std::filesystem::current_path(ec);
    cfg.root = pathutil::normalizeFsPath(root.string());

    std::string line;
    std::string section;
    while (std::getline(in, line)) {
        // strip UTF-8 BOM on the first line
        if (!line.empty() && static_cast<unsigned char>(line[0]) == 0xEF) line.erase(0, 3);
        const auto hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        line = trim(line);
        if (line.empty()) continue;

        if (line.front() == '[' && line.back() == ']') {
            section = toLower(trim(line.substr(1, line.size() - 2)));
            continue;
        }

        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            cfg.warnings.push_back("ignored line: " + line);
            continue;
        }
        const std::string key = toLower(trim(line.substr(0, eq)));
        const std::string value = unquote(trim(line.substr(eq + 1)));
        cfg.raw[section + "." + key] = value;
    }

    cfg.host = cfg.get("server", "host", "0.0.0.0");
    cfg.dhcp_port = cfg.get_int("server", "port", 67);
    cfg.server_ip = cfg.get("server", "server_ip");
    cfg.boot_root = resolvePath(cfg.get("server", "boot_root", "${SERVER_ROOT}/boot"), cfg.root);
    cfg.boot_file = pathutil::normalizeSlashes(cfg.get("server", "boot_file", "undionly.kpxe"));
    cfg.boot_file_efi = pathutil::normalizeSlashes(cfg.get("server", "boot_file_efi"));

    cfg.range_start = cfg.get("dhcp", "range_start");
    cfg.range_end = cfg.get("dhcp", "range_end");
    cfg.subnet_mask = cfg.get("dhcp", "subnet_mask", "255.255.255.0");
    cfg.router = cfg.get("dhcp", "router");
    cfg.dns = cfg.get("dhcp", "dns");
    cfg.domain = cfg.get("dhcp", "domain", "local.home");
    cfg.lease_time = cfg.get_int("dhcp", "lease_time", 21600);

    cfg.tftp_port = cfg.get_int("tftp", "port", 69);
    cfg.tftp_timeout = cfg.get_int("tftp", "timeout", 5);
    cfg.tftp_max_clients = cfg.get_int("tftp", "max_clients", 50);
    cfg.tftp_blksize = cfg.get_int("tftp", "blksize", 1456);

    cfg.http_port = cfg.get_int("http", "port", 80);
    cfg.http_root = resolvePath(cfg.get("http", "root", cfg.boot_root), cfg.root);

    cfg.log_level = toLower(cfg.get("logging", "level", "info"));
    cfg.log_file = resolvePath(cfg.get("logging", "file"), cfg.root);

    if (cfg.dhcp_port <= 0 || cfg.dhcp_port > 65535) {
        cfg.error = "invalid [server] port: " + std::to_string(cfg.dhcp_port);
        return cfg;
    }
    if (cfg.tftp_port <= 0 || cfg.tftp_port > 65535) {
        cfg.error = "invalid [tftp] port: " + std::to_string(cfg.tftp_port);
        return cfg;
    }
    if (cfg.http_port < 0 || cfg.http_port > 65535) {
        cfg.error = "invalid [http] port: " + std::to_string(cfg.http_port);
        return cfg;
    }

    ec.clear();
    const bool bootRootExists = std::filesystem::exists(cfg.boot_root, ec);
    ec.clear();
    const bool httpRootExists = std::filesystem::exists(cfg.http_root, ec);
    if (!bootRootExists) {
        cfg.warnings.push_back("boot root does not exist: " + cfg.boot_root);
    } else if (cfg.boot_file.compare(0, 7, "http://") == 0 || cfg.boot_file.compare(0, 8, "https://") == 0) {
        // HTTP/iPXE boot dosyası — yerel dosya yok; sunucu HTTP'yi servis edecek
        cfg.http_boot = true;
    } else if (!std::filesystem::exists(std::filesystem::path(cfg.boot_root) / cfg.boot_file, ec)) {
        cfg.warnings.push_back("boot file missing from boot root: " + cfg.boot_file);
    }

    // Detailed path validation logs (captured as warnings/info-like context for main logger).
    cfg.warnings.push_back("cfg: server_root=" + pathutil::normalizeFsPath(cfg.root));
    cfg.warnings.push_back("cfg: boot_root=" + pathutil::normalizeFsPath(cfg.boot_root) + " exists=" + (bootRootExists ? "yes" : "no"));
    cfg.warnings.push_back("cfg: http_root=" + pathutil::normalizeFsPath(cfg.http_root) + " exists=" + (httpRootExists ? "yes" : "no"));
    cfg.warnings.push_back("cfg: boot_file=" + cfg.boot_file + " http_boot=" + (cfg.http_boot ? "yes" : "no"));
    if (!cfg.http_boot && !cfg.boot_file.empty()) {
        ec.clear();
        const auto bfPath = std::filesystem::path(cfg.boot_root) / cfg.boot_file;
        const bool bfExists = std::filesystem::exists(bfPath, ec);
        cfg.warnings.push_back("cfg: local_boot_file_path=" + pathutil::normalizeFsPath(bfPath.string()) + " exists=" + (bfExists ? "yes" : "no"));
    }
    if (!cfg.boot_file_efi.empty()) {
        ec.clear();
        const auto efiPath = std::filesystem::path(cfg.boot_root) / cfg.boot_file_efi;
        const bool efiExists = std::filesystem::exists(efiPath, ec);
        cfg.warnings.push_back("cfg: efi_boot_file=" + cfg.boot_file_efi + " path=" + pathutil::normalizeFsPath(efiPath.string()) + " exists=" + (efiExists ? "yes" : "no"));
    }

    // Warn if the DHCP pool is on a different subnet than the server itself.
    if (!cfg.range_start.empty() && !cfg.server_ip.empty()) {
        const size_t rDot = cfg.range_start.find_last_of('.');
        const size_t sDot = cfg.server_ip.find_last_of('.');
        if (rDot != std::string::npos && sDot != std::string::npos &&
            cfg.range_start.substr(0, rDot) != cfg.server_ip.substr(0, sDot)) {
            cfg.warnings.push_back("DHCP range subnet (" + cfg.range_start.substr(0, rDot) +
                                  ".0) differs from server IP subnet (" +
                                  cfg.server_ip.substr(0, sDot) + ".0); clients may not boot");
        }
    }
    if (cfg.server_ip.empty()) {
        cfg.server_ip = detectLocalIp();
    }

    // Auto-derive DHCP pool/router/DNS from the server's own /24 when they are
    // not explicitly configured, so the server works out of the box on any LAN
    // instead of handing out addresses from a hard-coded 192.168.1.x range.
    const size_t lastDot = cfg.server_ip.find_last_of('.');
    if (lastDot != std::string::npos) {
        const std::string base = cfg.server_ip.substr(0, lastDot);
        if (cfg.range_start.empty()) cfg.range_start = base + ".100";
        if (cfg.range_end.empty()) cfg.range_end = base + ".199";
        if (cfg.router.empty()) cfg.router = base + ".1";
        if (cfg.dns.empty()) cfg.dns = base + ".1";
    }
    if (cfg.range_start.empty()) cfg.range_start = "192.168.1.100";
    if (cfg.range_end.empty()) cfg.range_end = "192.168.1.199";
    if (cfg.router.empty()) cfg.router = "192.168.1.1";
    if (cfg.dns.empty()) cfg.dns = "192.168.1.1";

    cfg.ok = true;
    return cfg;
}

std::string ConfigLoader::detectLocalIp() {
    socket_t sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (!socketValid(sock)) return "127.0.0.1";

    struct sockaddr_in probe;
    std::memset(&probe, 0, sizeof(probe));
    probe.sin_family = AF_INET;
    probe.sin_port = htons(53);
    ::inet_pton(AF_INET, "8.8.8.8", &probe.sin_addr);

    std::string result = "127.0.0.1";
    if (::connect(sock, reinterpret_cast<struct sockaddr*>(&probe), sizeof(probe)) == 0) {
        struct sockaddr_in local;
        std::memset(&local, 0, sizeof(local));
        socklen_type len = sizeof(local);
        if (::getsockname(sock, reinterpret_cast<struct sockaddr*>(&local), &len) == 0) {
            result = sockaddrToIp(local);
        }
    }
    socketClose(sock);
    return result;
}

} // namespace pxe
