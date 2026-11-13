#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <arpa/inet.h>

namespace pxe {

struct DhcpPacket {
    uint8_t op;
    uint8_t htype;
    uint8_t hlen;
    uint8_t hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint8_t options[312];
};

class DhcpProtocol {
public:
    static const uint8_t DHCPDISCOVER = 1;
    static const uint8_t DHCPOFFER = 2;
    static const uint8_t DHCPREQUEST = 3;
    static const uint8_t DHCPDECLINE = 4;
    static const uint8_t DHCPACK = 5;
    static const uint8_t DHCPNACK = 6;

    static DhcpPacket parse(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> createOffer(const DhcpPacket& pkt, const std::string& ip);
    static std::vector<uint8_t> createAck(const DhcpPacket& pkt, const std::string& ip);
};

class DHCPClient {
public:
    DHCPClient();
    ~DHCPClient();
    bool discover(const std::string& server_ip, std::string& offered_ip);
private:
    int sock_;
    ::sockaddr_in server_addr_;
    ::sockaddr_in cli_addr_;
};

} // namespace pxe
