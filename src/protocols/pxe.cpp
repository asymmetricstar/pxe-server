#include "protocols/pxe.hpp"
#include "utils/logger.hpp"
#include <cstring>

namespace pxe {

PXEBoot::PXEBoot() {
    state_ = State::DISCOVER;
}

bool PXEBoot::handleDhcpOffer(const std::string& offer_data) {
    if (state_ != State::DISCOVER) return false;

    // Parse DHCP OFFER
    if (!parseOffer(offer_data)) {
        Logger::instance().log(LogLevel::ERR, "Failed to parse DHCP OFFER");
        return false;
    }

    state_ = State::REQUEST;
    Logger::instance().log(LogLevel::INFO, "DHCP OFFER received, sending REQUEST");
    return true;
}

bool PXEBoot::handleDhcpAck(const std::string& ack_data) {
    if (state_ != State::REQUEST) return false;

    if (!parseAck(ack_data)) {
        Logger::instance().log(LogLevel::ERR, "Failed to parse DHCP ACK");
        return false;
    }

    state_ = State::ACTIVE;
    Logger::instance().log(LogLevel::INFO, "DHCP ACK received, TFTP boot file requested");
    return true;
}

bool PXEBoot::requestBootFile(const std::string& filename) {
    if (state_ != State::ACTIVE) {
        Logger::instance().log(LogLevel::ERR, "PXE not in ACTIVE state");
        return false;
    }
    Logger::instance().log(LogLevel::INFO, "Requesting boot file: " + filename);
    return true;
}

bool PXEBoot::parseOffer(const std::string& data) {
    return !data.empty();
}

bool PXEBoot::parseAck(const std::string& data) {
    return !data.empty();
}

} // namespace pxe
