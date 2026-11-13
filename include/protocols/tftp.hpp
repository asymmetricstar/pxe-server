#pragma once
#include <string>
#include <cstdint>
#include <vector>
#include <arpa/inet.h>

namespace pxe {

struct TftpPacket {
    uint16_t opcode;
    uint16_t id;
    std::string data;
};

class TftpProtocol {
public:
    static const uint16_t OP_READ = 1;
    static const uint16_t OP_WRITE = 2;
    static const uint16_t OP_ACK = 3;
    static const uint16_t OP_DATA = 4;
    static const uint16_t OP_ERROR = 5;
    static const uint16_t OP_OPTIONS = 6;
    static const uint16_t DATA_SIZE = 512;
    static const uint16_t ERROR_SIZE = 16;

    static TftpPacket parse(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> createAck(uint16_t id);
    static std::vector<uint8_t> createData(uint16_t id, const std::string& data);
    static std::vector<uint8_t> createError(uint16_t id, uint16_t code, const std::string& msg);
};

class TFTPClient {
public:
    TFTPClient();
    ~TFTPClient();
    bool transfer(const std::string& server_ip, const std::string& filename, int timeout_ms);
private:
    int sock_;
    ::sockaddr_in cli_addr_;
};

} // namespace pxe
