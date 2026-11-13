#pragma once
#include "compat/net.hpp"
#include "utils/config_loader.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace pxe {

/*
 * Small static HTTP server for the boot tree. "/" lists the boot root,
 * "/status" reports the running configuration and every other path is served
 * from the boot root (useful for large initramfs/ISO downloads over HTTP).
 */
class HTTPServer {
public:
    explicit HTTPServer(const Config& cfg);
    ~HTTPServer();

    bool start();
    void stop();
    bool running() const { return running_.load(); }
    int port() const { return cfg_.http_port; }
    const std::string& lastError() const { return error_; }

private:
    void loop();
    void handleClient(socket_t client, const struct sockaddr_in& from);
    std::string statusJson() const;
    std::string indexHtml() const;

    Config cfg_;
    socket_t sock_ = kInvalidSocket;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::string error_;
    std::chrono::steady_clock::time_point started_;

    /*
     * In-flight client handlers. Each connection runs on its own thread so a
     * slow client (or a big initramfs download) cannot stall every other
     * request; stop() waits for them before returning so no handler runs
     * against a closed/destroyed server.
     */
    std::mutex handlersMutex_;
    std::condition_variable handlersCv_;
    int handlers_ = 0;
};

} // namespace pxe
