#pragma once
#include "compat/net.hpp"
#include "utils/config_loader.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace pxe {

/*
 * DHCP server with PXE support. Answers DISCOVER/REQUEST with OFFER/ACK,
 * hands out addresses from the configured range and points clients at the
 * TFTP server and boot file so they can start the network boot.
 */
class DHCPServer {
public:
    explicit DHCPServer(const Config& cfg);
    ~DHCPServer();

    bool start();
    void stop();
    bool running() const { return running_.load(); }
    int port() const { return cfg_.dhcp_port; }
    const std::string& lastError() const { return error_; }

private:
    void loop();
    void handlePacket(const char* data, size_t len, const struct sockaddr_in& from);
    uint32_t allocate(const std::string& mac);
    void release(const std::string& mac);

    Config cfg_;
    socket_t sock_ = kInvalidSocket;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::string error_;

    std::mutex mutex_;
    std::map<std::string, uint32_t> leases_;   // mac -> ip (network order)
    uint32_t range_start_ = 0;
    uint32_t range_end_ = 0;
    uint32_t server_ip_ = 0;

    /*
     * Duplicate request suppression. PXE ROMs broadcast the same
     * DISCOVER/REQUEST twice within a couple of milliseconds; replying to
     * both makes the client receive duplicate OFFER/ACK datagrams.
     */
    std::mutex seenMutex_;
    std::map<std::string, std::chrono::steady_clock::time_point> seen_;
};

} // namespace pxe
