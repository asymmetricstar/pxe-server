#include "protocols/tftp.hpp"
#include "utils/logger.hpp"
#include <cstring>
#include <unistd.h>

namespace pxe {

TFTPClient::TFTPClient() {
    sock_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_ < 0) {
        Logger::instance().log(LogLevel::ERR, "TFTP socket creation failed");
        return;
    }
    memset(&cli_addr_, 0, sizeof(cli_addr_));
    cli_addr_.sin_family = AF_INET;
    cli_addr_.sin_port = htons(69);
    cli_addr_.sin_addr.s_addr = INADDR_ANY;
}

TFTPClient::~TFTPClient() {
    if (sock_ >= 0) close(sock_);
}

bool TFTPClient::transfer(const std::string& server_ip, const std::string& filename,
                           int timeout_ms) {
    if (sock_ < 0) return false;

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(69);
    inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr);

    // TFTP Read Request (opcode 1)
    char opcode = 1;
    std::string data;
    data += opcode;
    data += filename;
    data += '\0';

    std::string msg = "TFTP read request: " + filename;
    Logger::instance().log(LogLevel::DEBUG, msg);

    int sent = sendto(sock_, data.c_str(), data.size(), 0,
                      (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (sent < 0) {
        Logger::instance().log(LogLevel::ERR, "TFTP send failed");
        return false;
    }

    Logger::instance().log(LogLevel::INFO, "TFTP read request sent");
    return true;
}

} // namespace pxe
