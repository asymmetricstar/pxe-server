#pragma once
#include <string>
#include <map>
#include <vector>

namespace pxe {

/*
 * Runtime configuration. Everything the server needs comes from one INI file
 * (config/pxe.cfg by default) so DHCP, TFTP and HTTP never disagree about
 * ports or paths.
 */
struct Config {
    std::string file;            // path of the loaded config file
    std::string root;            // ${SERVER_ROOT}: parent of the config directory

    // DHCP / PXE
    std::string host = "0.0.0.0";
    int dhcp_port = 67;
    std::string server_ip;       // empty -> auto-detect local IPv4
    std::string boot_root;       // directory served over TFTP/HTTP
    std::string boot_file = "undionly.kpxe";    // BIOS NBP
    std::string boot_file_efi;                  // UEFI NBP (optional)
    bool http_boot = false;                 // iPXE HTTP boot modu

    std::string range_start = "192.168.1.100";
    std::string range_end = "192.168.1.199";
    std::string subnet_mask = "255.255.255.0";
    std::string router;
    std::string dns;
    std::string domain = "local.home";
    int lease_time = 21600;

    // TFTP
    int tftp_port = 69;
    int tftp_timeout = 5;        // seconds, retransmit timeout
    int tftp_max_clients = 50;
    int tftp_blksize = 1468;

    // HTTP
    int http_port = 80;
    std::string http_root;

    // Logging
    std::string log_level = "info";
    std::string log_file;

    std::map<std::string, std::string> raw;  // "section.key" -> value
    std::vector<std::string> warnings;
    bool ok = false;
    std::string error;

    std::string get(const std::string& section, const std::string& key,
                    const std::string& def = std::string()) const;
    int get_int(const std::string& section, const std::string& key, int def) const;
};

class ConfigLoader {
public:
    static Config load(const std::string& filename);

    // Expands ${SERVER_ROOT} and normalises separators.
    static std::string resolvePath(const std::string& path, const std::string& root);

    // Best-effort local IPv4 address (no packets are sent).
    static std::string detectLocalIp();
};

} // namespace pxe
