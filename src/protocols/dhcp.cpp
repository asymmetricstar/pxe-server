#include "protocols/dhcp.hpp"
#include "utils/logger.hpp"
#include <arpa/inet.h>
#include <cstring>

namespace pxe {

DHCPClient::DHCPClient() {
    sock_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_ < 0) {
        Logger::instance().log(LogLevel::ERR, "DHCP socket creation failed");
        return;
    }
    memset(&cli_addr_, 0, sizeof(cli_addr_));
    cli_addr_.sin_family = AF_INET;
    cli_addr_.sin_port = htons(68);
    cli_addr_.sin_addr.s_addr = INADDR_ANY;
}

DHCPClient::~DHCPClient() {
    if (sock_ >= 0) close(sock_);
}

bool DHCPClient::discover(const std::string& server_ip, std::string& offered_ip) {
    if (sock_ < 0) return false;
    offered_ip.clear();

    char packet[512];
    memset(packet, 0, sizeof(packet));
    // DHCP Discover message (simplified)
    packet[0] = 1;  // OP
    packet[1] = 1;  // BOOTREQUEST
    packet[2] = 0x11;  // HTYPE (Ethernet)
    packet[3] = 0x01;  // HLEN (1 byte)
    packet[4] = 0x01;  // HOPS

    inet_pton(AF_INET, server_ip.c_str(), &server_addr_.sin_addr);
    server_addr_.sin_family = AF_INET;
    server_addr_.sin_port = htons(67);

    std::string msg = "DHCP Discover to " + server_ip;
    Logger::instance().log(LogLevel::DEBUG, msg);

    int sent = sendto(sock_, packet, sizeof(packet), 0,
                      (struct sockaddr*)&server_addr_, sizeof(server_addr_));
    if (sent < 0) {
        Logger::instance().log(LogLevel::ERR, "DHCP send failed");
        return false;
    }

    // In real implementation, would receive DHCPOFFER here
    Logger::instance().log(LogLevel::INFO, "DHCP discover sent (offer handling not implemented)");
    return true;
}

} // namespace pxe
