#pragma once
#include "compat/net.hpp"
#include "utils/config_loader.hpp"

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace pxe {

/*
 * Read-only TFTP server (RFC 1350) with blksize/tsize/timeout negotiation
 * (RFC 2347/2348/2349). Every request is served from an ephemeral port, as
 * required by the protocol, so concurrent PXE clients do not block each other.
 */
class TFTPServer {
public:
    explicit TFTPServer(const Config& cfg);
    ~TFTPServer();

    bool start();
    void stop();
    bool running() const { return running_.load(); }
    int port() const { return cfg_.tftp_port; }
    const std::string& lastError() const { return error_; }

private:
    void loop();
    void serveRequest(const char* data, size_t len, const struct sockaddr_in& from);
    void transferFile(const struct sockaddr_in& client, const std::string& filename,
                      const std::map<std::string, std::string>& options,
                      const std::string& transferKey);
    void releaseTransfer(const std::string& transferKey);
    std::string resolvePath(const std::string& filename) const;

    Config cfg_;
    socket_t sock_ = kInvalidSocket;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<int> active_{0};
    std::string error_;

    /*
     * One transfer per (client TID, filename). Some PXE ROMs retransmit the
     * RRQ within a couple of milliseconds; spawning a second transfer for the
     * same request gives the client two different server TIDs, it locks onto
     * one and aborts the other (ERROR 5 / timeout), which shows up as an
     * intermittent PXE-E77.
     */
    std::mutex transfersMutex_;
    std::map<std::string, std::chrono::steady_clock::time_point> transfers_;
};

} // namespace pxe
