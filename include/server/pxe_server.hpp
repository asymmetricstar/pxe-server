#pragma once
#include "utils/config_loader.hpp"

#include <memory>
#include <string>

namespace pxe {
class DHCPServer;
class TFTPServer;
class HTTPServer;
}

namespace pxe_server {

class PXE {
public:
    PXE();
    ~PXE();

    bool init(const pxe::Config& cfg);
    bool start();
    void stop();
    bool running() const { return running_; }

    int dhcp_port() const;
    int tftp_port() const;
    int http_port() const;

private:
    pxe::Config cfg_;
    std::unique_ptr<pxe::DHCPServer> dhcp_;
    std::unique_ptr<pxe::TFTPServer> tftp_;
    std::unique_ptr<pxe::HTTPServer> http_;
    bool initialized_ = false;
    bool running_ = false;
};

} // namespace pxe_server
