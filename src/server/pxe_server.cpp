#include "server/pxe_server.hpp"
#include "server/dhcp_server.hpp"
#include "server/http_server.hpp"
#include "server/tftp_server.hpp"
#include "utils/iso_menu.hpp"
#include "utils/logger.hpp"

namespace pxe_server {

PXE::PXE() = default;

PXE::~PXE() {
    stop();
}

bool PXE::init(const pxe::Config& cfg) {
    cfg_ = cfg;
    // Build the ISO part of the boot menus before any client can ask for them.
    pxe::IsoMenu::refresh(cfg_);
    dhcp_ = std::make_unique<pxe::DHCPServer>(cfg_);
    tftp_ = std::make_unique<pxe::TFTPServer>(cfg_);
    http_ = std::make_unique<pxe::HTTPServer>(cfg_);
    initialized_ = true;
    return true;
}

bool PXE::start() {
    if (!initialized_) {
        pxe::Logger::instance().error("PXE::start() called before init()");
        return false;
    }

    int started = 0;
    if (dhcp_->start()) ++started;
    if (tftp_->start()) ++started;
    if (http_->start()) ++started;

    if (started == 0) {
        pxe::Logger::instance().error("No service could be started");
        return false;
    }
    if (started < 3) {
        pxe::Logger::instance().warn(std::to_string(started) +
                                     " of 3 services started; see errors above");
    }

    running_ = true;
    pxe::Logger::instance().info("PXE server ready: DHCP " + std::to_string(cfg_.dhcp_port) +
                                 ", TFTP " + std::to_string(cfg_.tftp_port) +
                                 ", HTTP " + std::to_string(cfg_.http_port) +
                                 " (boot: " + cfg_.boot_root + ")");
    return true;
}

void PXE::stop() {
    if (http_) http_->stop();
    if (tftp_) tftp_->stop();
    if (dhcp_) dhcp_->stop();
    running_ = false;
}

int PXE::dhcp_port() const { return cfg_.dhcp_port; }
int PXE::tftp_port() const { return cfg_.tftp_port; }
int PXE::http_port() const { return cfg_.http_port; }

} // namespace pxe_server
